#pragma once
// This is free and unencumbered software released into the public domain.
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.
// In jurisdictions that recognize copyright laws, the author or authors
// of this software dedicate any and all copyright interest in the
// software to the public domain. We make this dedication for the benefit
// of the public at large and to the detriment of our heirs and
// successors. We intend this dedication to be an overt act of
// relinquishment in perpetuity of all present and future rights to this
// software under copyright law.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
// For more information, please refer to <http://unlicense.org/>
//
// ***********************************************************************
//
//
// Simple Job Queue with priorities & child jobs
// - as soon as a job is added, it can be executed
// - if a job is added from inside a job it becomes a child job
// - When waiting for a job you also wait for all children
// - Implemented using c++11 thread/mutex/condition_variable
//	   on win32 CRITICAL_SECTION/Semaphore is used instead.
// - _not_ using jobstealing & per thread queues, so unsuitable for lots of very small jobs w. high contention
// - Different ways to store functions entrypoints
//     default: builtin fixed size (JQ_FUNCTION_SIZE) byte callback object. 
//              only for trivial types(memcpyable), compilation fails otherwise
//              never allocates, fails compilation if lambda contains more than JQ_FUNCTION_SIZE bytes.
//     define JQ_NO_LAMBDAS: raw function pointer with void* argument. Adds extra argument to JqAdd
//     define JQ_USE_STD_FUNCTION: use std::function. This will do a heap alloc if you capture too much (24b on osx, 16 on win32).
//     
// - Built in support for splitting ranges between jobs
//
//   example usage
//	
//		JqStart(2); //startup Jq with 2 worker threads
//		uint64_t nJobHandle = JqAdd( [](int start, int end) {} ), PRIORITY, NUM_JOBS, RANGE);
//		JqWait(nJobHandle);
//		JqStop(); //shutdown Jq
//
//		PRIORITY: 				[0-JQ_PRIORITY_MAX], lower priority gets executed earlier
//		NUM_JOBS(optional): 	number of times to spawn job
//		RANGE(optional): 		range to split between jobs

//	Todo:
//
//		Lockless:
//			Alloc/Free Job
//			Priority list
//			lockless finish list
//
//			


///////////////////////////////////////////////////////////////////////////////////////////
/// Configuration

#ifndef JQ_WORK_BUFFER_SIZE
#define JQ_WORK_BUFFER_SIZE (2048)
#endif

#ifndef JQ_PRIORITY_MAX
#define JQ_PRIORITY_MAX 8
#endif

#define JQ_NUM_PIPES JQ_PRIORITY_MAX  //for jq2 compat

#ifndef JQ_MAX_JOB_STACK
#define JQ_MAX_JOB_STACK 8
#endif

#ifndef JQ_DEFAULT_WAIT_TIME_US
#define JQ_DEFAULT_WAIT_TIME_US 100
#endif

#ifndef JQ_CACHE_LINE_SIZE
#define JQ_CACHE_LINE_SIZE 64
#endif

#ifndef JQ_API
#define JQ_API
#endif

#ifndef JQ_FUNCTION_SIZE
#define JQ_FUNCTION_SIZE 32
#endif

#ifndef JQ_MAX_THREADS
#define JQ_MAX_THREADS 64
#endif


#define JQ_ASSERT_SANITY 0



#include <type_traits>

//template stuff to make the job function accept to 0-2 arguments
template <typename T>
struct JqAdapt : public JqAdapt<decltype(&T::operator())>{};
template <typename C>
struct JqAdapt<void (C::*)(int, int) const>
{
	template<typename T>
	void call(T& t, int a, int b){ t(a,b);}
};
template <typename C>
struct JqAdapt<void (C::*)(int) const>
{
	template<typename T>
	void call(T& t, int a, int b){ t(a); }
};
template <typename C>
struct JqAdapt<void (C::*)() const>
{
	template<typename T>
	void call(T& t, int a, int b){ t();}
};

template <>
struct JqAdapt<void (*)(int, int)>
{
	template<typename T>
	void call(T& t, int a, int b){ t(a,b);}
};
template <>
struct JqAdapt<void (*)(int)>
{
	template<typename T>
	void call(T& t, int a, int b){ t(a); }
};
template <>
struct JqAdapt<void (*)()>
{
	template<typename T>
	void call(T& t, int a, int b){ t();}
};

//minimal lambda implementation without support for non-trivial types
//and fixed memory footprint
struct JqCallableBase {
	virtual void operator()(int begin, int end) = 0;
};
template <typename F>
struct JqCallable : JqCallableBase {
	F functor;
	JqCallable(F functor) : functor(functor) {}
	virtual void operator()(int a, int b) 
	{ 
		JqAdapt<F> X;
		X.call(functor,a,b);
	}
};
class JqFunction {
	union
	{
		char buffer[JQ_FUNCTION_SIZE];
		void* vptr;//alignment helper and a way to clear the vptr
	};
	JqCallableBase* Base()
	{
		return (JqCallableBase*)&buffer[0];
	}
public:
	template <typename F>
	JqFunction(F f) {
#if _MSC_VER < 1900	//HACK: msvc 2015 ALWAYS hits this :/
		static_assert(std::is_trivially_copyable<F>::value, "Only captures of trivial types supported. Use std::function if you think you need non-trivial types");
#endif
		static_assert(sizeof(JqCallable<F>) <= JQ_FUNCTION_SIZE, "Captured lambda is too big. Increase size or capture less");
#ifdef _WIN32
		static_assert(__alignof(F) <= __alignof(void*), "Alignment requirements too high");
#else
		static_assert(alignof(F) <= alignof(void*), "Alignment requirements too high");
#endif
		new (Base()) JqCallable<F>(f);
	}
	JqFunction(){}
	void Clear(){ vptr = 0;}
	void operator()(int a, int b) { (*Base())(a, b); }
};
#define JQ_CLEAR_FUNCTION(f) do{f.Clear();}while(0)



#include <stddef.h>
#include <stdint.h>

///////////////////////////////////////////////////////////////////////////////////////////
/// Interface


//  what to execute while wailing
#define JQ_WAITFLAG_EXECUTE_SUCCESSORS 0x1
#define JQ_WAITFLAG_EXECUTE_ANY 0x2
#define JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS 0x3
//  what to do when out of jobs
#define JQ_WAITFLAG_BLOCK 0x4
#define JQ_WAITFLAG_SLEEP 0x8
#define JQ_WAITFLAG_SPIN 0x10
#define JQ_WAITFLAG_IGNORE_CHILDREN 0x20



struct JqStats
{
	uint32_t nNumAdded;
	uint32_t nNumFinished;
	uint32_t nNumAddedSub;
	uint32_t nNumFinishedSub;
	uint32_t nNumCancelled;
	uint32_t nNumCancelledSub;
	uint32_t nNumLocks;
	uint32_t nNumWaitKicks;
	uint32_t nNumWaitCond;
	uint64_t nNextHandle;

	void Add(JqStats& Other)
	{
		nNumAdded += Other.nNumAdded;
		nNumFinished += Other.nNumFinished;
		nNumAddedSub += Other.nNumAddedSub;
		nNumFinishedSub += Other.nNumFinishedSub;
		nNumCancelled += Other.nNumCancelled;
		nNumCancelledSub += Other.nNumCancelledSub;
		nNumLocks += Other.nNumLocks;
		nNumWaitKicks += Other.nNumWaitKicks;
		nNumWaitCond += Other.nNumWaitCond;
	}

};

JQ_API uint64_t		JqSelf();
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1, uint64_t nParent = JqSelf());
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_BLOCK);
JQ_API void 		JqWait(uint64_t nJob, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_BLOCK, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void 		JqWaitAll();
//JQ_API void 		JqWaitAll(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_BLOCK, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API uint64_t		JqWaitAny(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_BLOCK, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API bool 		JqCancel(uint64_t nJob);
JQ_API void			JqExecuteChildren(uint64_t nJob);
JQ_API uint64_t 	JqGroupBegin(); //add a non-executing job to group all jobs added between begin/end
JQ_API void 		JqGroupEnd();
JQ_API bool 		JqIsDone(uint64_t nJob);
JQ_API bool 		JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlag);
JQ_API void			JqStart(int nNumWorkers);
JQ_API void			JqStart(int nNumWorkers, uint32_t nPipeConfigSize, uint8_t* pPipeConfig);
JQ_API void			JqSetThreadPipeConfig(uint8_t PipeConfig[JQ_NUM_PIPES]);
JQ_API int			JqNumWorkers();
JQ_API void 		JqStop();
JQ_API uint32_t		JqSelfJobIndex();
JQ_API int 			JqGetNumWorkers();
JQ_API void 		JqConsumeStats(JqStats* pStatsOut);
JQ_API bool			JqExecuteOne();
JQ_API void			JqDump();



///////////////////////////////////////////////////////////////////////////////////////////
/// Implementation

#ifdef JQ_IMPL
#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <unistd.h>
#define JQ_BREAK() __builtin_trap()
#define JQ_THREAD_LOCAL __thread
#define JQ_STRCASECMP strcasecmp
typedef uint64_t ThreadIdType;
#define JQ_USLEEP(us) usleep(us);
int64_t JqTicksPerSecond()
{
	static int64_t nTicksPerSecond = 0;	
	if(nTicksPerSecond == 0) 
	{
		mach_timebase_info_data_t sTimebaseInfo;	
		mach_timebase_info(&sTimebaseInfo);
		nTicksPerSecond = 1000000000ll * sTimebaseInfo.denom / sTimebaseInfo.numer;
	}
	return nTicksPerSecond;
}
int64_t JqTick()
{
	return mach_absolute_time();
}
#elif defined(_WIN32)
#define JQ_BREAK() __debugbreak()
#define JQ_THREAD_LOCAL __declspec(thread)
#define JQ_STRCASECMP _stricmp
typedef uint32_t ThreadIdType;
#define JQ_USLEEP(us) JqUsleep(us);
#include <windows.h>
int64_t JqTicksPerSecond()
{
	static int64_t nTicksPerSecond = 0;	
	if(nTicksPerSecond == 0) 
	{
		QueryPerformanceFrequency((LARGE_INTEGER*)&nTicksPerSecond);
	}
	return nTicksPerSecond;
}
int64_t JqTick()
{
	int64_t ticks;
	QueryPerformanceCounter((LARGE_INTEGER*)&ticks);
	return ticks;
}
void JqUsleep(__int64 usec) 
{ 
	if(usec > 20000)
	{
		Sleep((DWORD)(usec/1000));
	}
	else if(usec >= 1000)
	{
#ifdef _DURANGO
		Sleep((DWORD)(usec/1000));
#else
		timeBeginPeriod(1);
		Sleep((DWORD)(usec/1000));
		timeEndPeriod(1);
#endif
	}
	else
	{
		Sleep(0);
	}
}
#endif

#ifndef JQ_THREAD
#include <thread>
#define JQ_THREAD std::thread
#define JQ_THREAD_CREATE(pThread) do{} while(0)
#define JQ_THREAD_DESTROY(pThread) do{} while(0)
#define JQ_THREAD_START(pThread, entry, index) do{ *pThread = std::thread(entry, index); } while(0)
#define JQ_THREAD_JOIN(pThread) do{(pThread)->join();}while(0)
#endif

#ifdef JQ_NO_ASSERT
#define JQ_ASSERT(a) do{}while(0)
#else
#define JQ_ASSERT(a) do{if(!(a)){JQ_BREAK();} }while(0)
#endif

#ifdef JQ_ASSERT_LOCKS
#define JQ_ASSERT_LOCKED() do{if(0 == JqHasLock){JQ_BREAK();}}while(0)
#define JQ_ASSERT_NOT_LOCKED()  do{if(0 != JqHasLock){JQ_BREAK();}}while(0)
#define JQ_ASSERT_LOCK_ENTER() do{JqHasLock++;}while(0)
#define JQ_ASSERT_LOCK_LEAVE()  do{JqHasLock--;}while(0)
#else
#define JQ_ASSERT_LOCKED() do{}while(0)
#define JQ_ASSERT_NOT_LOCKED()  do{}while(0)
#define JQ_ASSERT_LOCK_ENTER() do{}while(0)
#define JQ_ASSERT_LOCK_LEAVE()  do{}while(0)
#endif

#define JQ_NUM_JOBS (JQ_WORK_BUFFER_SIZE-1)

#ifdef JQ_MICROPROFILE
#define JQ_MICROPROFILE_SCOPE(a,c) MICROPROFILE_SCOPEI("JQ",a,c)
#else
#define JQ_MICROPROFILE_SCOPE(a,c) do{}while(0)
#endif

#ifdef JQ_MICROPROFILE_VERBOSE
#define JQ_MICROPROFILE_VERBOSE_SCOPE(a,c) MICROPROFILE_SCOPEI("JQ",a,c)
#else
#define JQ_MICROPROFILE_VERBOSE_SCOPE(a,c) do{}while(0)
#endif

#define JQ_PRIORITY_SIZE (JQ_PRIORITY_MAX+1)
#define JQ_PRIORITY_ARRAY_SIZE (JQ_PRIORITY_SIZE)



struct JqMutexLock;

void 		JqStart(int nNumWorkers);
void 		JqCheckFinished(uint64_t nJob);
uint16_t 	JqIncrementStarted(uint64_t nJob);
void 		JqIncrementFinished(uint64_t nJob);
void		JqAttachChild(uint64_t nParentJob, uint64_t nChildJob);
uint64_t 	JqDetachChild(uint64_t nChildJob);
void 		JqExecuteJob(uint64_t nJob, uint16_t nJobSubIndex);
uint16_t 	JqTakeJob(uint16_t* pJobSubIndex, uint32_t nNumPrio, uint8_t* pPrio);
uint16_t 	JqTakeChildJob(uint64_t nJob, uint16_t* pJobSubIndex);
void 		JqWorker(int nThreadId);
uint64_t 	JqNextHandle(uint64_t nJob);
void 		JqWaitAll();
void 		JqPriorityListAdd(uint16_t nJobIndex);
void 		JqPriorityListRemove(uint16_t nJobIndex);
uint64_t 	JqSelf();
uint32_t	JqSelfJobIndex();
int 		JqGetNumWorkers();
bool 		JqPendingJobs(uint64_t nJob);
void 		JqSelfPush(uint64_t nJob, uint32_t nJobIndex);
void 		JqSelfPop(uint64_t nJob);
uint64_t 	JqFindHandle(JqMutexLock& Lock);
bool		JqExecuteOne();


struct JqSelfStack
{
	uint64_t nJob;
	uint32_t nJobIndex;
};

JQ_THREAD_LOCAL JqSelfStack JqSelfStack[JQ_MAX_JOB_STACK] = {{0}};
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;
JQ_THREAD_LOCAL uint32_t JqHasLock = 0;

#ifndef JG_LT_WRAP
#define JQ_LT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<0)
#define JQ_LE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<=0)
#define JQ_GE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))>=0)
#define JQ_GT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))>0)
#define JQ_LT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<0)
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<=0)
#define JQ_GE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))>=0)
#define JQ_GT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))>0)
#endif

struct JqJob
{
	JqFunction Function;

	uint64_t nStartedHandle;
	uint64_t nFinishedHandle;

	int32_t nRange;

	uint16_t nNumJobs;
	uint16_t nNumStarted;
	uint16_t nNumFinished;

	//parent/child tree
	uint16_t nParent;
	uint16_t nFirstChild;
	uint16_t nSibling;

	//priority linked list
	uint16_t nLinkNext;
	uint16_t nLinkPrev;
	uint8_t nPrio;
	uint8_t nWaiters;
	uint8_t nWaitersWas;
	// int8_t nWaitIndex;

	// uint32_t nSignalCount;//debug

#ifdef JQ_ASSERT_SANITY
	int nTag;
#endif
};

#ifdef _WIN32
#define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_16 __declspec(align(16))
#else
#define JQ_ALIGN_CACHELINE __attribute__((__aligned__(JQ_CACHE_LINE_SIZE)))
#define JQ_ALIGN_16 __attribute__((__aligned__(16)))
#endif

// #define JQ_PAD_SIZE(type) (JQ_CACHE_LINE_SIZE - (sizeof(type)%JQ_CACHE_LINE_SIZE))
// #define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))

#ifndef _WIN32
#include <pthread.h>
#endif
#include <atomic>


struct JqMutex
{
	JqMutex();
	~JqMutex();
	void Lock();
	void Unlock();
#ifdef _WIN32
	CRITICAL_SECTION CriticalSection;
#else
	pthread_mutex_t Mutex;
#endif
};

struct JqConditionVariable
{
	JqConditionVariable();
	~JqConditionVariable();
	void Wait(JqMutex& Mutex);
	void NotifyOne();
	void NotifyAll();
#ifdef _WIN32
	CONDITION_VARIABLE Cond;
#else
	pthread_cond_t Cond;
#endif
};



struct JqSemaphore
{
	JqSemaphore();
	~JqSemaphore();
	void Signal(uint32_t nCount);
	void Wait();

	void Init(int nMaxCount);

#ifdef _WIN32
	HANDLE Handle;
	LONG nMaxCount;
#else
	JqMutex Mutex;
	JqConditionVariable Cond;
	uint32_t nReleaseCount;
	uint32_t nMaxCount;	
#endif
};

#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS
//note: This is just arbitrary: given a suffiently random priority setup you might need to bump this.

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore[JQ_MAX_SEMAPHORES];
	JqMutex Mutex;
	JqConditionVariable WaitCond;

	uint64_t m_SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t m_PipeNumSemaphores[JQ_NUM_PIPES];
	uint8_t m_PipeToSemaphore[JQ_NUM_PIPES][JQ_MAX_SEMAPHORES];
	uint8_t m_SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t m_SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int		m_ActiveSemaphores;

	uint8_t m_nNumPipes[JQ_MAX_THREADS];
	uint8_t m_PipeList[JQ_MAX_THREADS][JQ_NUM_PIPES];
	uint8_t m_SemaphoreIndex[JQ_MAX_THREADS];


	JQ_THREAD WorkerThreads[JQ_MAX_THREADS];




	int nNumWorkers;
	int nStop;
	int nTotalWaiting;
	uint64_t nNextHandle;
	uint32_t nFreeJobs;

	JqJob Jobs[JQ_WORK_BUFFER_SIZE];
	uint16_t nPrioListHead[JQ_PRIORITY_ARRAY_SIZE];
	uint16_t nPrioListTail[JQ_PRIORITY_ARRAY_SIZE];

	JqState_t()
		:nNumWorkers(0)
	{
	}


	JqStats Stats;
} JqState;


struct JqMutexLock
{
	JqMutex& Mutex;
	JqMutexLock(JqMutex& Mutex)
		:Mutex(Mutex)
	{
		Lock();
	}
	~JqMutexLock()
	{
		Unlock();
	}
	void Lock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexLock", 0x992233);
		Mutex.Lock();
		JqState.Stats.nNumLocks++;
		JQ_ASSERT_LOCK_ENTER();
	}
	void Unlock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
		JQ_ASSERT_LOCK_LEAVE();
		Mutex.Unlock();
	}
};

JQ_THREAD_LOCAL int JqSpinloop = 0; //prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumPipes = 0;
JQ_THREAD_LOCAL uint8_t g_JqPipes[JQ_NUM_PIPES] = { 0 };


void JqStart(int nNumWorkers)
{
	JqStart(nNumWorkers, 0, 0);
}

void JqStart(int nNumWorkers, uint32_t nPipeConfigSize, uint8_t* pPipeConfig)
{
#if 0
	//verify macros
	uint64_t t0 = 0xf000000000000000;
	uint64_t t1 = 0;
	uint64_t t2 = 0x1000000000000000;
	JQ_ASSERT(JQ_LE_WRAP(t0, t0));
	JQ_ASSERT(JQ_LE_WRAP(t1, t1));
	JQ_ASSERT(JQ_LE_WRAP(t2, t2));
	JQ_ASSERT(JQ_LE_WRAP(t0, t1));
	JQ_ASSERT(!JQ_LE_WRAP(t1, t0));
	JQ_ASSERT(JQ_LE_WRAP(t1, t2));
	JQ_ASSERT(!JQ_LE_WRAP(t2, t1));
	JQ_ASSERT(JQ_LE_WRAP(t0, t2));
	JQ_ASSERT(!JQ_LE_WRAP(t2, t0));

	JQ_ASSERT(!JQ_LT_WRAP(t0, t0));
	JQ_ASSERT(!JQ_LT_WRAP(t1, t1));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t2));
	JQ_ASSERT(JQ_LT_WRAP(t0, t1));
	JQ_ASSERT(!JQ_LT_WRAP(t1, t0));
	JQ_ASSERT(JQ_LT_WRAP(t1, t2));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t1));
	JQ_ASSERT(JQ_LT_WRAP(t0, t2));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t0));
#endif

	JQ_ASSERT_NOT_LOCKED();

	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&(uint64_t)&JqState) == 0);
	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.nNumWorkers == 0);


	memset(JqState.m_PipeList, 0xff, sizeof(JqState.m_PipeList));
	memset(JqState.m_nNumPipes, 0, sizeof(JqState.m_nNumPipes));
	memset(JqState.m_SemaphoreMask, 0, sizeof(JqState.m_SemaphoreMask));
	memset(JqState.m_PipeNumSemaphores, 0, sizeof(JqState.m_PipeNumSemaphores));
	memset(JqState.m_PipeToSemaphore, 0, sizeof(JqState.m_PipeToSemaphore));
	memset(JqState.m_SemaphoreClients, 0, sizeof(JqState.m_SemaphoreClients));
	memset(JqState.m_SemaphoreClientCount, 0, sizeof(JqState.m_SemaphoreClientCount));
	JqState.m_ActiveSemaphores = 0;

	for (int i = 0; i < nNumWorkers; ++i)
	{
		uint8_t* pPipes = pPipeConfig + JQ_NUM_PIPES * i;
		uint8_t nNumActivePipes = 0;
		uint64_t PipeMask = 0;
		static_assert(JQ_NUM_PIPES < 64, "wont fit in 64bit mask");
		if (nPipeConfigSize)
		{
			for (uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
			{
				if (pPipes[j] != 0xff)
				{
					JqState.m_PipeList[i][nNumActivePipes++] = pPipes[j];
					JQ_ASSERT(pPipes[j] < JQ_NUM_PIPES);
					PipeMask |= 1llu << pPipes[j];
				}
			}
		}
		else
		{
			for (uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
			{
				JqState.m_PipeList[i][nNumActivePipes++] = j;
				PipeMask |= 1llu << j;
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.m_nNumPipes[i] = nNumActivePipes;
		int nSelectedSemaphore = -1;
		for (int j = 0; j < JqState.m_ActiveSemaphores; ++j)
		{
			if (JqState.m_SemaphoreMask[j] == PipeMask)
			{
				nSelectedSemaphore = j;
				break;
			}
		}
		if (-1 == nSelectedSemaphore)
		{
			JQ_ASSERT(JqState.m_ActiveSemaphores < JQ_MAX_SEMAPHORES);
			nSelectedSemaphore = JqState.m_ActiveSemaphores++;
			JqState.m_SemaphoreMask[nSelectedSemaphore] = PipeMask;
			for (uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
			{
				if (PipeMask & (1llu << j))
				{
					JQ_ASSERT(JqState.m_PipeNumSemaphores[j] < JQ_MAX_SEMAPHORES);
					JqState.m_PipeToSemaphore[j][JqState.m_PipeNumSemaphores[j]++] = nSelectedSemaphore;
				}
			}
		}
		JQ_ASSERT(JqState.m_SemaphoreClientCount[nSelectedSemaphore] < JQ_MAX_SEMAPHORES);
		JqState.m_SemaphoreClients[nSelectedSemaphore][JqState.m_SemaphoreClientCount[nSelectedSemaphore]++] = i;
		JqState.m_SemaphoreIndex[i] = nSelectedSemaphore;
	}

	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		JqState.Semaphore[i].Init(JqState.m_SemaphoreClientCount[i] ? JqState.m_SemaphoreClientCount[i] : 1);
	}



#if 0
	for (uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
	{
		printf("pipe %d : ", i);
		for (uint32_t j = 0; j < JqState.m_PipeNumSemaphores[i]; ++j)
		{
			printf("%d, ", JqState.m_PipeToSemaphore[i][j]);
		}
		printf("\n");
	}


	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		if (JqState.m_SemaphoreClientCount[i])
		{
			printf("Semaphore %d, clients %d Mask %llx :: ", i, JqState.m_SemaphoreClientCount[i], JqState.m_SemaphoreMask[i]);
			for (uint32_t j = 0; j < JqState.m_SemaphoreClientCount[i]; ++j)
			{
				printf("%d, ", JqState.m_SemaphoreClients[i][j]);
			}
			printf("\n");
		}
	}

	for (int i = 0; i < nNumWorkers; ++i)
	{
		printf("worker %d Sema %d Mask %llx :: Pipes ", i, JqState.m_SemaphoreIndex[i], JqState.m_SemaphoreMask[JqState.m_SemaphoreIndex[i]]);
		for (uint32_t j = 0; j < JqState.m_nNumPipes[i]; ++j)
		{
			printf("%d, ", JqState.m_PipeList[i][j]);
		}
		printf("\n");

	}
#endif
	JqState.nTotalWaiting = 0;
	JqState.nNumWorkers = nNumWorkers;
	JqState.nStop = 0;
	JqState.nTotalWaiting = 0;
	for(int i = 0; i < JQ_WORK_BUFFER_SIZE; ++i)
	{
		JqState.Jobs[i].nStartedHandle = 0;
		JqState.Jobs[i].nFinishedHandle = 0;	
	}
	memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.nFreeJobs = JQ_NUM_JOBS;
	JqState.nNextHandle = 1;
	JqState.Stats.nNumFinished = 0;
	JqState.Stats.nNumLocks = 0;
	JqState.Stats.nNumWaitKicks = 0;
	JqState.Stats.nNumWaitCond = 0;

	for(int i = 0; i < nNumWorkers; ++i)
	{
		JQ_THREAD_CREATE(&JqState.WorkerThreads[i]);
		JQ_THREAD_START(&JqState.WorkerThreads[i], JqWorker, (i));
	}
}
int JqNumWorkers()
{
	return JqState.nNumWorkers;
}

void JqStop()
{
	JqWaitAll();
	JqState.nStop = 1;
	for (int i = 0; i < JqState.m_ActiveSemaphores; ++i)
	{
		JqState.Semaphore[i].Signal(JqState.nNumWorkers);
	}
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JQ_THREAD_JOIN(&JqState.WorkerThreads[i]);
		JQ_THREAD_DESTROY(&JqState.WorkerThreads[i]);
	}
	JqState.nNumWorkers = 0;
}

void JqSetThreadPipeConfig(uint8_t PipeConfig[JQ_NUM_PIPES])
{
	JQ_ASSERT(g_nJqNumPipes == 0); // its not supported to change this value, nor is it supported to set it on worker threads. set on init instead.
	uint32_t nNumActivePipes = 0;
	for (uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
	{
		if (PipeConfig[j] != 0xff)
		{
			g_JqPipes[nNumActivePipes++] = PipeConfig[j];
			JQ_ASSERT(PipeConfig[j] < JQ_NUM_PIPES);
		}
	}
	g_nJqNumPipes = nNumActivePipes;
}


void JqConsumeStats(JqStats* pStats)
{
	JqMutexLock lock(JqState.Mutex);
	*pStats = JqState.Stats;
	JqState.Stats.nNumAdded = 0;
	JqState.Stats.nNumFinished = 0;
	JqState.Stats.nNumAddedSub = 0;
	JqState.Stats.nNumFinishedSub = 0;
	JqState.Stats.nNumCancelled = 0;
	JqState.Stats.nNumCancelledSub = 0;
	JqState.Stats.nNumLocks = 0;
	JqState.Stats.nNumWaitKicks = 0;
	JqState.Stats.nNumWaitCond = 0;
	JqState.Stats.nNextHandle = JqState.nNextHandle;
}


void JqCheckFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint32_t nIndex = nJob % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].nFinishedHandle, nJob));
	JQ_ASSERT(nJob == JqState.Jobs[nIndex].nStartedHandle);
	if(0 == JqState.Jobs[nIndex].nFirstChild && JqState.Jobs[nIndex].nNumFinished == JqState.Jobs[nIndex].nNumJobs)
	{
		uint16_t nParent = JqState.Jobs[nIndex].nParent;
		if(nParent)
		{
			uint64_t nParentHandle = JqDetachChild(nJob);
			JqState.Jobs[nIndex].nParent = 0;
			JqCheckFinished(nParentHandle);
		}
		JqState.Jobs[nIndex].nFinishedHandle = JqState.Jobs[nIndex].nStartedHandle;
		JQ_CLEAR_FUNCTION(JqState.Jobs[nIndex].Function);

		JqState.Stats.nNumFinished++;
		//kick waiting threads.
		int8_t nWaiters = JqState.Jobs[nIndex].nWaiters;
		if(nWaiters != 0)
		{
			JqState.Stats.nNumWaitKicks++;
			JqState.WaitCond.NotifyAll();
			JqState.Jobs[nIndex].nWaiters = 0;
			JqState.Jobs[nIndex].nWaitersWas = nWaiters;
		}
		else
		{
			JqState.Jobs[nIndex].nWaitersWas = 0xff;
		}
		JqState.nFreeJobs++;
	}
}

uint16_t JqIncrementStarted(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nIndex = nJob % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(JqState.Jobs[nIndex].nNumJobs > JqState.Jobs[nIndex].nNumStarted);
	uint16_t nSubIndex = JqState.Jobs[nIndex].nNumStarted++;
	if(JqState.Jobs[nIndex].nNumStarted == JqState.Jobs[nIndex].nNumJobs)
	{
		JqPriorityListRemove(nIndex);
	}
	return nSubIndex;
}
void JqIncrementFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
	uint16_t nIndex = nJob % JQ_WORK_BUFFER_SIZE; 
	JqState.Jobs[nIndex].nNumFinished++;
	JqState.Stats.nNumFinishedSub++;
	JqCheckFinished(nJob);
}


void JqAttachChild(uint64_t nParentJob, uint64_t nChildJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nParentIndex = nParentJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nChildIndex = nChildJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nFirstChild = JqState.Jobs[nParentIndex].nFirstChild;
	JqState.Jobs[nChildIndex].nParent = nParentIndex;
	JqState.Jobs[nChildIndex].nSibling = nFirstChild;
	JqState.Jobs[nParentIndex].nFirstChild = nChildIndex;

	JQ_ASSERT(JqState.Jobs[nParentIndex].nFinishedHandle != nParentJob);
	JQ_ASSERT(JqState.Jobs[nParentIndex].nStartedHandle == nParentJob);
}

uint64_t JqDetachChild(uint64_t nChildJob)
{
	uint16_t nChildIndex = nChildJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nParentIndex = JqState.Jobs[nChildIndex].nParent;
	JQ_ASSERT(JqState.Jobs[nChildIndex].nNumFinished == JqState.Jobs[nChildIndex].nNumJobs);
	JQ_ASSERT(JqState.Jobs[nChildIndex].nNumFinished == JqState.Jobs[nChildIndex].nNumStarted);
	if(0 == nParentIndex)
	{
		return 0;
	}

	JQ_ASSERT(nParentIndex != 0);
	JQ_ASSERT(JqState.Jobs[nChildIndex].nFirstChild == 0);
	uint16_t* pChildIndex = &JqState.Jobs[nParentIndex].nFirstChild;
	JQ_ASSERT(JqState.Jobs[nParentIndex].nFirstChild != 0); 
	while(*pChildIndex != nChildIndex)
	{
		JQ_ASSERT(JqState.Jobs[*pChildIndex].nSibling != 0);
		pChildIndex = &JqState.Jobs[*pChildIndex].nSibling;

	}
	JQ_ASSERT(pChildIndex);
	*pChildIndex = JqState.Jobs[nChildIndex].nSibling;
	return JqState.Jobs[nParentIndex].nStartedHandle;
}

//splits range evenly to jobs
int JqGetRangeStart(int nIndex, int nFraction, int nRemainder)
{
	int nStart = 0;
	if(nIndex > 0)
	{
		nStart = nIndex * nFraction;
		if(nRemainder <= nIndex)
			nStart += nRemainder;
		else
			nStart += nIndex;
	}
	return nStart;
}

void JqSelfPush(uint64_t nJob, uint32_t nSubIndex)
{	
	JqSelfStack[JqSelfPos].nJob = nJob;
	JqSelfStack[JqSelfPos].nJobIndex = nSubIndex;
	JqSelfPos++;
}

void JqSelfPop(uint64_t nJob)
{
	JQ_ASSERT(JqSelfPos != 0);
	JqSelfPos--;
	JQ_ASSERT(JqSelfStack[JqSelfPos].nJob == nJob);	
}

uint32_t JqSelfJobIndex()
{
	JQ_ASSERT(JqSelfPos != 0);
	return JqSelfStack[JqSelfPos-1].nJobIndex;
}

int JqGetNumWorkers()
{
	return JqState.nNumWorkers;
}

void JqExecuteJob(uint64_t nJob, uint16_t nSubIndex)
{
	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JqSelfPush(nJob, nSubIndex);
	uint16_t nWorkIndex = nJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nNumJobs = JqState.Jobs[nWorkIndex].nNumJobs;
	int nRange = JqState.Jobs[nWorkIndex].nRange;
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;	
	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
	JqState.Jobs[nWorkIndex].Function(nStart, nEnd);
	JqSelfPop(nJob);
	//JqIncrementFinished(nJob);
}


uint16_t JqTakeJob(uint16_t* pSubIndex, uint32_t nNumPrio, uint8_t* pPrio)
{
	JQ_ASSERT_LOCKED();
	if(nNumPrio)
	{
		for(uint32_t i = 0; i < nNumPrio; i++)
		{
			uint16_t nIndex = JqState.nPrioListHead[pPrio[i]];
			if(nIndex)
			{
				*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
				return nIndex;
			}
		}
	}
	else
	{
		for(int i = 0; i < JQ_PRIORITY_ARRAY_SIZE; ++i)
		{
			uint16_t nIndex = JqState.nPrioListHead[i];
			if(nIndex)
			{
				*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
				return nIndex;
			}
		}
	}
	return 0;
}
#ifdef JQ_ASSERT_SANITY
void JqTagChildren(uint16_t nRoot)
{
	for(int i = 1; i < JQ_WORK_BUFFER_SIZE; ++i)
	{
		JQ_ASSERT(JqState.Jobs[i].nTag == 0);
		if(nRoot == i)
		{
			JqState.Jobs[i].nTag = 1;
		}
		else
		{
			int nParent = JqState.Jobs[i].nParent;
			while(nParent)
			{
				if(nParent == nRoot)
				{
					JqState.Jobs[i].nTag = 1;
					break;
				}
				nParent = JqState.Jobs[nParent].nParent;
			}
		}
	}

}
void JqCheckTagChildren(uint16_t nRoot)
{
	for(int i = 1; i < JQ_WORK_BUFFER_SIZE; ++i)
	{
		JQ_ASSERT(JqState.Jobs[i].nTag == 0);
		bool bTagged = false;
		if(nRoot == i)
			bTagged = false;
		else
		{
			int nParent = JqState.Jobs[i].nParent;
			while(nParent)
			{
				if(nParent == nRoot)
				{
					bTagged = true;
					break;
				}
				nParent = JqState.Jobs[nParent].nParent;
			}
		}
		JQ_ASSERT(bTagged == (1==JqState.Jobs[i].nTag));
	}
}

void JqLoopChildren(uint16_t nRoot)
{
	int nNext = JqState.Jobs[nRoot].nFirstChild;
	while(nNext != nRoot && nNext)
	{
		while(JqState.Jobs[nNext].nFirstChild)
			nNext = JqState.Jobs[nNext].nFirstChild;
		JQ_ASSERT(JqState.Jobs[nNext].nTag == 1);
		JqState.Jobs[nNext].nTag = 0;
		if(JqState.Jobs[nNext].nSibling)
			nNext = JqState.Jobs[nNext].nSibling;
		else
		{
			//search up
			nNext = JqState.Jobs[nNext].nParent;
			while(nNext != nRoot)
			{
				JQ_ASSERT(JqState.Jobs[nNext].nTag == 1);
				JqState.Jobs[nNext].nTag = 0;
				if(JqState.Jobs[nNext].nSibling)
				{
					nNext = JqState.Jobs[nNext].nSibling;
					break;
				}
				else
				{
					nNext = JqState.Jobs[nNext].nParent;
				}

			}
		}
	}

	JQ_ASSERT(JqState.Jobs[nRoot].nTag == 1);
	JqState.Jobs[nRoot].nTag = 0;
}
#endif


//Depth first. Once a node is visited all child nodes have been visited
uint16_t JqTreeIterate(uint64_t nJob, uint16_t nCurrent)
{
	JQ_ASSERT(nJob);
	uint16_t nRoot = nJob % JQ_WORK_BUFFER_SIZE;
	if(nRoot == nCurrent)
	{
		while(JqState.Jobs[nCurrent].nFirstChild)
			nCurrent = JqState.Jobs[nCurrent].nFirstChild;
	}
	else
	{
		//once here all child nodes _have_ been processed.
		if(JqState.Jobs[nCurrent].nSibling)
		{
			nCurrent = JqState.Jobs[nCurrent].nSibling;
			while(JqState.Jobs[nCurrent].nFirstChild) //child nodes first.
				nCurrent = JqState.Jobs[nCurrent].nFirstChild;
		}
		else
		{
			nCurrent = JqState.Jobs[nCurrent].nParent;
		}
	}
	return nCurrent;
}


//this code is O(n) where n is the no. of child nodes.
//I wish this could be written in a simpler way
uint16_t JqTakeChildJob(uint64_t nJob, uint16_t* pSubIndexOut)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeChildJob", 0xff);
	JQ_ASSERT_LOCKED();
#if JQ_ASSERT_SANITY
	{
		//verify that the child iteration is sane
		for(int i = 0; i < JQ_WORK_BUFFER_SIZE; ++i)
		{
			JqState.Jobs[i].nTag = 0;
		}
		JqTagChildren(nJob%JQ_WORK_BUFFER_SIZE);
		uint16_t nIndex = nJob%JQ_WORK_BUFFER_SIZE;
		uint16_t nRootIndex = nJob % JQ_WORK_BUFFER_SIZE;
		do
		{
			nIndex = JqTreeIterate(nJob, nIndex);
			JQ_ASSERT(JqState.Jobs[nIndex].nTag == 1);
			JqState.Jobs[nIndex].nTag = 0;
		}while(nIndex != nRootIndex);
		for(int i = 0; i < JQ_WORK_BUFFER_SIZE; ++i)
		{
			JQ_ASSERT(JqState.Jobs[i].nTag == 0);
		}
	}
#endif

	uint16_t nRoot = nJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nIndex = nRoot;

	do
	{
		nIndex = JqTreeIterate(nJob, nIndex);
		JQ_ASSERT(nIndex);

		if(JqState.Jobs[nIndex].nNumStarted < JqState.Jobs[nIndex].nNumJobs)
		{
			*pSubIndexOut = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
			return nIndex;
		}
	}while(nIndex != nRoot);

	return 0;
}



bool JqExecuteOne()
{
	uint16_t nSubIndex = 0;
	uint16_t nWork;
	{
		JqMutexLock lock(JqState.Mutex);
		nWork = JqTakeJob(&nSubIndex, g_nJqNumPipes, g_JqPipes);
	}
	if(!nWork)
		return false;
	JqExecuteJob(JqState.Jobs[nWork].nStartedHandle, nSubIndex);
	{
		JqMutexLock lock(JqState.Mutex);
		JqIncrementFinished(JqState.Jobs[nWork].nStartedHandle);
	}
	return true;
}

void JqWorker(int nThreadId)
{

	uint8_t* pPipes = JqState.m_PipeList[nThreadId];
	uint32_t nNumPipes = JqState.m_nNumPipes[nThreadId];
	g_nJqNumPipes = nNumPipes; //even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqPipes, pPipes, nNumPipes);
	int nSemaphoreIndex = JqState.m_SemaphoreIndex[nThreadId];


#if MICROPROFILE_ENABLED
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker)-1, "JqWorker %d %08llx", nThreadId, JqState.m_SemaphoreMask[nSemaphoreIndex]);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.nStop)
	{
		uint16_t nWork = 0;
		do
		{
			uint16_t nSubIndex = 0;
			{
				JqMutexLock lock(JqState.Mutex);
				if(nWork)
				{
					JqIncrementFinished(JqState.Jobs[nWork].nStartedHandle);
				}
				nSubIndex = 0;
				nWork = JqTakeJob(&nSubIndex, nNumPipes, pPipes);
			}
			if(!nWork)
			{
				break;
			}
			JqExecuteJob(JqState.Jobs[nWork].nStartedHandle, nSubIndex);
		}while(1);

		JqState.Semaphore[nSemaphoreIndex].Wait();
	}
#ifdef JQ_MICROPROFILE
	MicroProfileOnThreadExit();
#endif
}
uint64_t JqNextHandle(uint64_t nJob)
{
	nJob++;
	if(0 == (nJob%JQ_WORK_BUFFER_SIZE))
	{
		nJob++;
	}
	return nJob;
}
uint64_t JqFindHandle(JqMutexLock& Lock)
{
	JQ_ASSERT_LOCKED();
	while(!JqState.nFreeJobs)
	{
		if(JqSelfPos < JQ_MAX_JOB_STACK)
		{
			JQ_MICROPROFILE_SCOPE("AddExecute", 0xff); // if this starts happening the job queue size should be increased..
			uint16_t nSubIndex = 0;
			uint16_t nIndex = JqTakeJob(&nSubIndex, 0, nullptr);
			if(nIndex)
			{
				Lock.Unlock();
				JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
				Lock.Lock();
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			}
		}
		else
		{
			JQ_BREAK(); //out of job queue space. increase JQ_WORK_BUFFER_SIZE or create fewer jobs
		}
	}
	JQ_ASSERT(JqState.nFreeJobs != 0);
	uint64_t nNextHandle = JqState.nNextHandle;
	uint16_t nCount = 0;
	while(JqPendingJobs(nNextHandle))
	{
		nNextHandle = JqNextHandle(nNextHandle);
		JQ_ASSERT(nCount++ < JQ_WORK_BUFFER_SIZE);
	}
	JqState.nNextHandle = JqNextHandle(nNextHandle);
	JqState.nFreeJobs--;	
	return nNextHandle;
}

JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nWaitFlag)
{
	uint64_t nJob = JqAdd(JobFunc, nPrio, nNumJobs, nRange);
	JqWait(nJob, nWaitFlag);
}

bool JqCancel(uint64_t nJob)
{
	JqMutexLock Lock(JqState.Mutex);
	uint16_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;

	JqJob* pEntry = &JqState.Jobs[nIndex];
	if(pEntry->nStartedHandle != nJob)
		return false;
	if(pEntry->nNumStarted != 0)
		return false;
	uint32_t nNumJobs = pEntry->nNumJobs;
	pEntry->nNumJobs = 0;

	JqPriorityListRemove(nIndex);
	JqCheckFinished(nJob);
	JQ_ASSERT(JqIsDone(nJob));

	JqState.Stats.nNumCancelled++;
	JqState.Stats.nNumCancelledSub += nNumJobs;
	return true;
}




uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint64_t nParent)
{
	JQ_ASSERT(nPrio < JQ_PRIORITY_SIZE);
	JQ_ASSERT(JqState.nNumWorkers);
	JQ_ASSERT(nNumJobs);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	if(nNumJobs < 0)
	{
		nNumJobs = JqState.nNumWorkers;
	}
	uint64_t nNextHandle = 0;
	{
		JqMutexLock Lock(JqState.Mutex);
		nNextHandle = JqFindHandle(Lock);
		uint16_t nIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;

		JqJob* pEntry = &JqState.Jobs[nIndex];
		JQ_ASSERT(JQ_LT_WRAP(pEntry->nFinishedHandle, nNextHandle));
		pEntry->nStartedHandle = nNextHandle;
		JQ_ASSERT(nNumJobs <= 0xffff);
		pEntry->nNumJobs = (uint16_t)nNumJobs;
		pEntry->nNumStarted = 0;
		pEntry->nNumFinished = 0;
		pEntry->nRange = nRange;
		uint64_t nParentHandle = nParent;
		pEntry->nParent = nParentHandle % JQ_WORK_BUFFER_SIZE;
		if(pEntry->nParent)
		{
			JqAttachChild(nParentHandle, nNextHandle);
		}
		pEntry->Function = JobFunc;
		pEntry->nPrio = nPrio;
		pEntry->nWaiters = 0;
		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += nNumJobs;
		JqPriorityListAdd(nIndex);
	}


	uint32_t nNumSema = JqState.m_PipeNumSemaphores[nPrio];
	for (uint32_t i = 0; i < nNumSema; ++i)
	{
		int nSemaIndex = JqState.m_PipeToSemaphore[nPrio][i];
		JqState.Semaphore[nSemaIndex].Signal(nNumJobs);
	}

	//JqState.SemaphoreAll.Signal(nNumJobs);
	//if(eJobType == EJQ_TYPE_SHORT)
	//{
	//	JqState.SemaphoreSmall.Signal(nNumJobs);
	//}
	return nNextHandle;
}

void JqDump()
{

}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;
	uint64_t nFinishedHandle = JqState.Jobs[nIndex].nFinishedHandle;
	uint64_t nStartedHandle = JqState.Jobs[nIndex].nStartedHandle;
	JQ_ASSERT(JQ_LE_WRAP(nFinishedHandle, nStartedHandle));
	int64_t nDiff = (int64_t)(nFinishedHandle - nJob);
	JQ_ASSERT((nDiff >= 0) == JQ_LE_WRAP(nJob, nFinishedHandle));
	return JQ_LE_WRAP(nJob, nFinishedHandle);
}

bool JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlags)
{
	bool bIsDone = JqIsDone(nJob);
	if(!bIsDone && 0 != (nWaitFlags & JQ_WAITFLAG_IGNORE_CHILDREN))
	{
		uint64_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;
		return JqState.Jobs[nIndex].nNumJobs == JqState.Jobs[nIndex].nNumFinished;
	}
	return bIsDone;
}


bool JqPendingJobs(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;
	JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].nFinishedHandle, JqState.Jobs[nIndex].nStartedHandle));
	return JqState.Jobs[nIndex].nFinishedHandle != JqState.Jobs[nIndex].nStartedHandle;
}


void JqWaitAll()
{
	uint16_t nIndex = 0;
	while(JqState.nFreeJobs != JQ_NUM_JOBS)
	{
		uint16_t nSubIndex = 0;
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
			{
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);		
			}
			nIndex = JqTakeJob(&nSubIndex, 0, nullptr);
		}
		if(nIndex)
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}
		else
		{
			JQ_USLEEP(1000);
		}	
	}
	if(nIndex)
	{
		JqMutexLock lock(JqState.Mutex);
		JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);		
	}
}


void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	uint16_t nIndex = 0;
	if(JqIsDone(nJob))
	{
		return;
	}
	while(!JqIsDoneExt(nJob, nWaitFlag))
	{

		uint16_t nSubIndex = 0;
		if((nWaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag)) 
				return;
			nIndex = JqTakeChildJob(nJob, &nSubIndex);
			if(!nIndex)
			{
				nIndex = JqTakeJob(&nSubIndex, g_nJqNumPipes, g_JqPipes);
			}
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag)) 
				return;
			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		else if(0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag)) 
				return;
			nIndex = JqTakeJob(&nSubIndex, g_nJqNumPipes, g_JqPipes);
		}
		else
		{
			JQ_BREAK();
		}
		if(!nIndex)
		{
			JQ_ASSERT(0 != (nWaitFlag & (JQ_WAITFLAG_SLEEP|JQ_WAITFLAG_BLOCK|JQ_WAITFLAG_SPIN)));
			if(nWaitFlag & JQ_WAITFLAG_SPIN)
			{
				uint64_t nTick = JqTick();
				uint64_t nTicksPerSecond = JqTicksPerSecond();
				do
				{
					uint32_t result = 0;
					for(uint32_t i = 0; i < 1000; ++i)
					{
						result |= i << (i&7); //do something.. whatever
					}
					JqSpinloop |= result; //write it somewhere so the optimizer can't remote it
				}while( (1000000ull*(JqTick()-nTick)) / nTicksPerSecond < nUsWaitTime);


			}
			else if(nWaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(nUsWaitTime);
			}
			else
			{
				JqMutexLock lock(JqState.Mutex);
				if(JqIsDoneExt(nJob, nWaitFlag))
				{
					return;
				}
				uint16_t nJobIndex = nJob % JQ_WORK_BUFFER_SIZE;
				JqState.Jobs[nJobIndex].nWaiters++;
				JqState.Stats.nNumWaitCond++;
				JqState.WaitCond.Wait(JqState.Mutex);
				JqState.Jobs[nJobIndex].nWaiters--;
			}
		}
		else
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}
	}
	if(nIndex)
	{
		JqMutexLock lock(JqState.Mutex);
		JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
	}

}


void JqExecuteChildren(uint64_t nJob)
{
	if(JqIsDone(nJob))
	{
		return;
	}

	uint16_t nIndex = 0;
	do 
	{		
		uint16_t nSubIndex = 0;
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);

			if(JqIsDone(nJob)) 
				return;

			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		if(nIndex)
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}

	}while(nIndex);
}


void JqWaitAll(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	for(uint32_t i = 0; i < nNumJobs; ++i)
	{
		if(!JqIsDone(pJobs[i]))
		{
			JqWait(pJobs[i], nWaitFlag, nUsWaitTime);
		}
	}
}

uint64_t JqWaitAny(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	JQ_BREAK(); //todo
	return 0;
}


uint64_t JqGroupBegin()
{
	JqMutexLock Lock(JqState.Mutex);
	uint64_t nNextHandle = JqFindHandle(Lock);
	uint16_t nIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
	JqJob* pEntry = &JqState.Jobs[nIndex];
	JQ_ASSERT(JQ_LE_WRAP(pEntry->nFinishedHandle, nNextHandle));
	pEntry->nStartedHandle = nNextHandle;
	pEntry->nNumJobs = 1;
	pEntry->nNumStarted = 1;
	pEntry->nNumFinished = 0;
	pEntry->nRange = 0;
	uint64_t nParentHandle = JqSelf();
	pEntry->nParent = nParentHandle % JQ_WORK_BUFFER_SIZE;
	if(pEntry->nParent)
	{
		JqAttachChild(nParentHandle, nNextHandle);
	}
	JQ_CLEAR_FUNCTION(pEntry->Function);
	pEntry->nPrio = 7;
	pEntry->nWaiters = 0;
	JqSelfPush(nNextHandle, 0);
	return nNextHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);

	JqMutexLock lock(JqState.Mutex);
	JqIncrementFinished(nJob);
}



uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1].nJob : 0;
}


void JqPriorityListAdd(uint16_t nIndex)
{
	uint8_t nPrio = JqState.Jobs[nIndex].nPrio;
	JQ_ASSERT(JqState.Jobs[nIndex].nLinkNext == 0);
	JQ_ASSERT(JqState.Jobs[nIndex].nLinkPrev == 0);
	uint16_t nTail = JqState.nPrioListTail[nPrio];
	if(nTail != 0)
	{
		JQ_ASSERT(JqState.Jobs[nTail].nLinkNext == 0);
		JqState.Jobs[nTail].nLinkNext = nIndex;
		JqState.Jobs[nIndex].nLinkPrev = nTail;
		JqState.Jobs[nIndex].nLinkNext = 0;
		JqState.nPrioListTail[nPrio] = nIndex;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListHead[nPrio] == 0);
		JqState.nPrioListHead[nPrio] = nIndex;
		JqState.nPrioListTail[nPrio] = nIndex;
		JqState.Jobs[nIndex].nLinkNext = 0;
		JqState.Jobs[nIndex].nLinkPrev = 0;
	}
}
void JqPriorityListRemove(uint16_t nIndex)
{
	uint8_t nPrio = JqState.Jobs[nIndex].nPrio;
	uint16_t nNext = JqState.Jobs[nIndex].nLinkNext;
	uint16_t nPrev = JqState.Jobs[nIndex].nLinkPrev;
	JqState.Jobs[nIndex].nLinkNext = 0;
	JqState.Jobs[nIndex].nLinkPrev = 0;

	if(nNext != 0)
	{
		JqState.Jobs[nNext].nLinkPrev = nPrev;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListTail[nPrio] == nIndex);
		JqState.nPrioListTail[nPrio] = nPrev;
	}
	if(nPrev != 0)
	{
		JqState.Jobs[nPrev].nLinkNext = nNext;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListHead[nPrio] == nIndex);
		JqState.nPrioListHead[nPrio] = nNext;
	}

}

#ifdef _WIN32
JqMutex::JqMutex()
{
	InitializeCriticalSection(&CriticalSection);
}

JqMutex::~JqMutex()
{
	DeleteCriticalSection(&CriticalSection);
}

void JqMutex::Lock()
{
	EnterCriticalSection(&CriticalSection);
}

void JqMutex::Unlock()
{
	LeaveCriticalSection(&CriticalSection);
}


JqConditionVariable::JqConditionVariable()
{
	InitializeConditionVariable(&Cond);
}

JqConditionVariable::~JqConditionVariable()
{
	//?
}

void JqConditionVariable::Wait(JqMutex& Mutex)
{
	SleepConditionVariableCS(&Cond, &Mutex.CriticalSection, INFINITE);
}

void JqConditionVariable::NotifyOne()
{
	WakeConditionVariable(&Cond);
}

void JqConditionVariable::NotifyAll()
{
	WakeAllConditionVariable(&Cond);
}

JqSemaphore::JqSemaphore()
{
	Handle = 0;
}
JqSemaphore::~JqSemaphore()
{
	if(Handle)
	{
		CloseHandle(Handle);
	}

}
void JqSemaphore::Init(int nCount)
{
	if(Handle)
	{
		CloseHandle(Handle);
		Handle = 0;
	}
	nMaxCount = nCount;
	Handle = CreateSemaphoreEx(NULL, 0, nCount*2, NULL, 0, SEMAPHORE_ALL_ACCESS);	
}


void JqSemaphore::Signal(uint32_t nCount)
{
	if(nCount > (uint32_t)nMaxCount)
		nCount = nMaxCount;
	BOOL r = ReleaseSemaphore(Handle, nCount, 0);
}

void JqSemaphore::Wait()
{
	JQ_MICROPROFILE_SCOPE("Wait", 0xc0c0c0);
	DWORD r = WaitForSingleObject((HANDLE)Handle, INFINITE);
	JQ_ASSERT(WAIT_OBJECT_0 == r);
}
#else
JqMutex::JqMutex()
{
	pthread_mutex_init(&Mutex, 0);
}

JqMutex::~JqMutex()
{
	pthread_mutex_destroy(&Mutex);
}

void JqMutex::Lock()
{
	pthread_mutex_lock(&Mutex);
}

void JqMutex::Unlock()
{
	pthread_mutex_unlock(&Mutex);
}


JqConditionVariable::JqConditionVariable()
{
	pthread_cond_init(&Cond, 0);
}

JqConditionVariable::~JqConditionVariable()
{
	pthread_cond_destroy(&Cond);
}

void JqConditionVariable::Wait(JqMutex& Mutex)
{
	pthread_cond_wait(&Cond, &Mutex.Mutex);
}

void JqConditionVariable::NotifyOne()
{
	pthread_cond_signal(&Cond);
}

void JqConditionVariable::NotifyAll()
{
	pthread_cond_broadcast(&Cond);
}

JqSemaphore::JqSemaphore()
{
	nMaxCount = 0xffffffff;
	nReleaseCount = 0;
}
JqSemaphore::~JqSemaphore()
{

}
void JqSemaphore::Init(int nCount)
{
	nMaxCount = nCount;
}
void JqSemaphore::Signal(uint32_t nCount)
{
	{
		JqMutexLock l(Mutex);
		uint32_t nCurrent = nReleaseCount;
		if(nCurrent + nCount > nMaxCount)
			nCount = nMaxCount - nCurrent;
		nReleaseCount += nCount;
		JQ_ASSERT(nReleaseCount <= nMaxCount);
		if(nReleaseCount == nMaxCount)
		{
			Cond.NotifyAll();
		}
		else
		{
			for(uint32_t i = 0; i < nReleaseCount; ++i)
			{
				Cond.NotifyOne();
			}
		}
	}
}

void JqSemaphore::Wait()
{
	JQ_MICROPROFILE_SCOPE("Wait", 0xc0c0c0);
	JqMutexLock l(Mutex);
	while(!nReleaseCount)
	{
		Cond.Wait(Mutex);
	}
	nReleaseCount--;
}

#endif
#endif
