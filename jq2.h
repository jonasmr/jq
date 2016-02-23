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


#ifndef JQ_NUM_PIPES
#define JQ_NUM_PIPES 4
#endif

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

#include "jqpipe.h"

#ifdef JQ_NO_LAMBDA
typedef void (*JqFunction)(void*, int, int);
#ifdef JQ_USE_STD_FUNCTION
#error "JQ_NO_LAMBDA and JQ_USE_STD_FUNCTION cannot both be defined"
#endif
#else
#ifdef JQ_USE_STD_FUNCTION
#include <functional>
typedef std::function<void (int,int) > JqFunction;
#else
#include <type_traits>
//minimal lambda implementation without support for non-trivial types
//and fixed memory footprint
struct JqCallableBase {
	virtual void operator()(int begin, int end) = 0;
};
template <typename F>
struct JqCallable : JqCallableBase {
	F functor;
	JqCallable(F functor) : functor(functor) {}
	virtual void operator()(int a, int b) { functor(a, b); }
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
		static_assert(sizeof(F) <= JQ_FUNCTION_SIZE, "Captured lambda is too big. Increase size or capture less");
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
#endif
#endif

#ifndef JQ_CLEAR_FUNCTION
#define JQ_CLEAR_FUNCTION(f) do{f = nullptr;}while(0)
#endif



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
	uint32_t nNumLocks;
	uint32_t nNumWaitKicks;
	uint32_t nNumWaitCond;
	uint32_t nMemoryUsed;
};

#define JQ_DEFAULT_WAIT_FLAG (JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SPIN)

JQ_API uint64_t		JqSelf();
#ifdef JQ_NO_LAMBDA
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs = 1, int nRange = -1, uint64_t nParent = JqSelf());
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs = 1, int nRange = -1, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG);
#else
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1, uint64_t nParent = JqSelf());
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG);
#endif
JQ_API void 		JqWait(uint64_t nJob, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void			JqWaitAll();
JQ_API void 		JqWaitAll(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API uint64_t		JqWaitAny(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
//JQ_API void			JqExecuteChildren(uint64_t nJob);
JQ_API uint64_t 	JqGroupBegin(); //add a non-executing job to group all jobs added between begin/end
JQ_API void 		JqGroupEnd();
JQ_API bool 		JqIsDone(uint64_t nJob);
JQ_API bool 		JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlag);
JQ_API void 		JqStart(int nNumWorkers);
JQ_API void 		JqStart(int nNumWorkers, uint32_t nPipeConfigSize, uint8_t* pPipeConfig);
JQ_API int			JqNumWorkers();
JQ_API void 		JqStop();
JQ_API uint32_t		JqSelfJobIndex();
JQ_API int 			JqGetNumWorkers();
JQ_API void 		JqConsumeStats(JqStats* pStatsOut);
JQ_API bool			JqExecuteOne(int nShortOnly);
JQ_API void 		JqStartSentinel(int nTimeout);

///////////////////////////////////////////////////////////////////////////////////////////
/// Implementation

#ifdef JQ_IMPL
#if defined(__APPLE__)
#include <mach/mach_time.h>
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
#define snprintf _snprintf
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

#define JQ_NUM_JOBS (JQ_TREE_BUFFER_SIZE2-1)

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
#define JQ_PRIORITY_ARRAY_SIZE (2*JQ_PRIORITY_SIZE)



struct JqMutexLock;

void 		JqStart(int nNumWorkers);
void 		JqStart(int nNumWorkers, uint32_t nPipeConfigSize, uint8_t* pPipeConfig);
void 		JqCheckFinished(uint64_t nJob);
uint16_t 	JqIncrementStarted(uint64_t nJob);
void 		JqIncrementFinished(uint64_t nJob);
void		JqAttachChild(uint64_t nParentJob, uint64_t nChildJob);
uint64_t 	JqDetachChild(uint64_t nChildJob);
void 		JqExecuteJob(uint64_t nJob, uint16_t nJobSubIndex);
//uint16_t 	JqTakeChildJob(uint64_t nJob, uint16_t* pJobSubIndex);
void 		JqWorker(int nThreadId);
uint64_t 	JqNextHandle(uint64_t nJob);
void 		JqWaitAll();
void 		JqPriorityListAdd(uint16_t nJobIndex);
void 		JqPriorityListRemove(uint16_t nJobIndex);
uint64_t 	JqSelf();
uint32_t	JqSelfJobIndex();
int 		JqGetNumWorkers();
bool 		JqPendingJobs(uint64_t nJob);
void 		JqSelfPush(uint64_t nHandle, uint32_t nJobIndex);
void 		JqSelfPop(uint64_t nHandle);
uint64_t 	JqFindHandle(JqMutexLock& Lock);
bool		JqExecuteOne(int nShortOnly);
int JqGetRangeStart(int nIndex, int nFraction, int nRemainder);



struct JqSelfStack
{
	uint64_t nHandle;
	uint32_t nJobIndex;
};

JQ_THREAD_LOCAL JqSelfStack JqSelfStack[JQ_MAX_JOB_STACK];
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;
JQ_THREAD_LOCAL uint32_t JqHasLock = 0;
JQ_THREAD_LOCAL int JqSpinloop = 0; //prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumPipes = 0;
JQ_THREAD_LOCAL uint8_t g_JqPipes[JQ_NUM_PIPES] = {0};

#ifndef JQ_LT_WRAP
#define JQ_LT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<0)
#define JQ_LT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<0)
#define JQ_LE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<=0)
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<=0)
#endif

#define JQ_TREE_BUFFER_SIZE2 (1<<JQ_PIPE_EXTERNAL_ID_BITS) // note: this should match with nExternalId size in JqPipe
struct JqJob2
{
	JqFunction Function;
#ifdef JQ_NO_LAMBDA
	void* pArg;
#endif

	uint64_t nFinishedHandle;
	uint64_t nStartedHandle;
	std::atomic<JqPipeHandle> PipeHandle;
	uint64_t nRoot;
	//uint16_t nLockIndex;
	uint16_t nParent;
	uint16_t nFirstChild;
	uint16_t nSibling;
};

// struct JqJob
// {
// 	JqFunction Function;
// #ifdef JQ_NO_LAMBDA
// 	void* pArg;
// #endif

// 	uint64_t nStartedHandle;
// 	uint64_t nFinishedHandle;
// 	int32_t nRange;
// 	uint16_t nNumJobs;
// 	uint16_t nNumStarted;
// 	uint16_t nNumFinished;

// #ifdef JQ_ASSERT_SANITY
// 	int nTag;
// #endif
// };


#define JQ_PAD_SIZE(type) (JQ_CACHE_LINE_SIZE - (sizeof(type)%JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_16 __declspec(align(16))

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


#define JQ_STATS(a) do{a;}while(0)



// JqPipeHandle JqPipeAdd(JqPipe* pPipe, JqFunction JobFunc, int nNumJobs, int nRange);
// {
// 	JQ_ASSERT(nNumJobs);
// 	if(nRange < 0)
// 	{
// 		nRange = nNumJobs;
// 	}
// 	if(nNumJobs < 0)
// 	{
// 		nNumJobs = JqState.nNumWorkers;
// 	}
// 	uint64_t nNextHandle = 0;
// 	do
// 	{
// 		nNextHandle = pPipe->nPut.fetch_add(1);
// 		nNextHandle = & 0xffffffff;
// 		if(nNextHandle)
// 		{
// 			uint16_t nJobIndex = nNextHandle % JQ_TREE_BUFFER_SIZE2;
// 			JqJob* pJob = pPipe->Jobs[nJobIndex];
// 			JqJobState State;
// 			do
// 			{
// 				State = JqJobStateLoad(pJob);
// 				if(State.nStartedHandled == State.nFinishedHandle &&
// 				   State.nNumJobs == State.nNumStarted &&
// 				   State.nNumJobs == State.nNumFinished)
// 				{
// 					JqState New;
// 					New.nNumJobs = (uint32_t)nNumJobs;
// 					New.nNumStarted = 0;
// 					New.nUnused = 0;
// 					New.nStartedHandle = nNextHandle;
// 					New.nNumFinished = 0;
// 					New.nRange = (uint32_t)nRange;
// 					New.nFinishedHandle =  State.nFinishedHandle;
// 					if(JqJobStateCompareAndSwap(pJob, New, State))
// 					{
// 						return (nNextHandle<<8) | pPipe->nPipeId;
// 					}
// 				}
// 				else
// 				{
// 					break;//still running, bail out and take next
// 				}
// 			}while(1);
// 		}
// 	}while(1);
// }


// //internal
// bool JqPipeStartInternal(JqPipe* pPipe, uint32_t nHandleInternal, uint16_t* pSubJob, bool* pLast)
// {
// 	uint16_t nJobIndex = nGet % JQ_TREE_BUFFER_SIZE2;
// 	JqJob* pJob = pPipe->Jobs[nJobIndex];
// 	JqJobState State;
// 	do
// 	{
// 		State = JqJobStateLoad(pJob);
// 		if(State.nStartedHandle == nHandleInternal && State.nNumJobs > State.nNumStarted)
// 		{
// 			JqJobState NewState = State;
// 			NewState.nNumStarted = State.nNumstarted+1;
// 			bool bLast = State.nNumstarted + 1 == State.nNumJobs;
// 			if(JqJobStateCompareAndSwap(pJob, NewState, State))
// 			{
// 				*pSubJob = NewState.nNumStarted;
// 				*pLast = bLast;
// 				return true;
// 			}
// 			else
// 			{
// 				continue;
// 			}

// 		}
// 		else
// 		{
// 			return false;
// 		}
// 	}while(1);
// }

// bool JqPipeFinishInternal(JqPipe* pPipe, uint32_t nHandleInternal)
// {
// 	uint16_t nJobIndex = nGet % JQ_TREE_BUFFER_SIZE2;
// 	JqJob* pJob = pPipe->Jobs[nJobIndex];
// 	JqJobState State;
// 	do
// 	{
// 		State = JqJobStateLoad(pJob);
// 		JQ_ASSERT(State.nStartedHandle == nHandleInternal)
// 		JQ_ASSERT(State.nNumJobs < State.nNumFinished)
// 		JqState NewState = State;
// 		NewState.nNumFinished = State.nNumFinished+1;
// 		bool bLast = NewState.nNumFinished == State.nNumJobs;
// 		if(JqJobStateCompareAndSwap(pJob, NewState, State))
// 		{
// 			return bLast;
// 		}
// 	}while(1);
// }


// JqPipeExecuteResult JqPipeExecuteInternal(JqPipe* pPipe, uint32_t nHandleInternal)
// {
// 	uint16_t nSubJob;
// 	bool bIsLast = false;
// 	if(JqPipeStartInternal(pPipe, nHandleInternal, &nSubJob, &bLast))
// 	{
// 		if(bLast)
// 		{
// 			pPipe->nGet.fetch_add(1);
// 		}
// 		//execute.

// 		{
// 			JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
// 			JqState State = JqJobStateLoad(pJob);

// 			pPipe->StartJobFunc(pJob, nSubJob, State.nRange);
// 			uint16_t nNumJobs = State.nNumJobs;
// 			int nRange = State.nRange;
// 			int nFraction = nRange / nNumJobs;
// 			int nRemainder = nRange - nFraction * nNumJobs;	
// 			int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
// 			int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
// 			pJob->Function(nStart, nEnd);
// 		}
// 		//finish

// 		if(JqPipeFinishInternal(pPipe, nHandleInternal))
// 		{
// 			return EJQ_EXECUTE_FINISHED;
// 		}
// 		else
// 		{
// 			return EJQ_EXECUTE_SUCCES;
// 		}
// 	}

// 	return EJQ_EXCUTE_FAIL;

// }


// JqPipeExecuteResult JqPipeExecute(JqPipe* pPipe, JqPipeHandle Job)
// {
// 	if(0 != Job.HandleInt)
// 	{
// 		return JqPipeExecuteInternal(pPipe, Job.HandleInt);
// 	}
// 	else
// 	{
// 		uint64_t nPut = pPipe->nPut.load();
// 		uint64_t nGet = pPipe->nGet.load();
// 		while(nPut != nGet)
// 		{
// 			uint32_t nHandle = (uint32_t)nGet;
// 			JqPipeExecuteResult eRes = JqPipeExecuteInternal(pPipe, nHandle);
// 			if(eRes != EJQ_EXCUTE_FAIL)
// 			{
// 				return eRes;
// 			}
// 			nPut = pPipe->nPut.load();
// 			nGet = pPipe->nGet.load();
// 		}
// 	}
// 	return EJQ_EXCUTE_FAIL;
// }

// bool JqPipeIsDone(JqPipe* pPipe, uint64_t nJob)
// {
// 	uint64_t nIndex = JqPipeIndex(nJob);
// 	uint64_t nFinishedHandle = pPipe->nFinishedHandle.load();
// 	uint64_t nStartedHandle = pPipe->nStartedHandle.load();
// 	JQ_ASSERT(JQ_LE_WRAP(nFinishedHandle, nStartedHandle));
// 	int64_t nDiff = (int64_t)(nFinishedHandle - nJob);
// 	JQ_ASSERT((nDiff >= 0) == JQ_LE_WRAP(nJob, nFinishedHandle));
// 	return JQ_LE_WRAP(nJob, nFinishedHandle);

// }




struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore;
	char pad0[ JQ_PAD_SIZE(JqSemaphore) ]; 

	JqMutex Mutex;
	char pad1[ JQ_PAD_SIZE(JqMutex) ]; 

	JqConditionVariable WaitCond;
	char pad2[ JQ_PAD_SIZE(JqConditionVariable) ];


	JQ_THREAD WorkerThreads[JQ_MAX_THREADS];

	JQ_THREAD SentinelThread;
	int nNumWorkers;
	int nStop;
	int nStopSentinel;
	int nTotalWaiting;
	int nSentinelRunning;
	std::atomic<int> nFrozen;
	uint64_t nNextHandle;
	uint32_t nFreeJobs;
	uint64_t nShortOnlyMask;
	std::atomic<uint64_t> nJobsFinished;

	uint8_t m_nNumPipes[JQ_MAX_THREADS];
	uint8_t m_PipeList[JQ_MAX_THREADS][JQ_NUM_PIPES];

	JqPipe 			m_Pipes[JQ_NUM_PIPES];

	std::atomic<uint64_t> 	m_NextHandle;
	JqJob2 			m_Jobs2[JQ_TREE_BUFFER_SIZE2];

	JqState_t()
		:nNumWorkers(0)
	{
	}


	JqStats Stats;
} JqState;


struct JqMutexLock
{
	bool bIsLocked;
	JqMutex& Mutex;
	JqMutexLock(JqMutex& Mutex)
		:Mutex(Mutex)
	{
		bIsLocked = false;
		Lock();
	}
	~JqMutexLock()
	{
		if(bIsLocked)
		{
			Unlock();
		}
	}
	void Lock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexLock", 0x992233);
		Mutex.Lock();
		JqState.Stats.nNumLocks++;
		JQ_ASSERT_LOCK_ENTER();
		bIsLocked = true;
	}
	void Unlock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
		JQ_ASSERT_LOCK_LEAVE();
		Mutex.Unlock();
		bIsLocked = false;
	}
};

void JqCheckFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	JQ_ASSERT(nIndex < JQ_TREE_BUFFER_SIZE2);
	if(0 == JqState.m_Jobs2[nIndex].nFirstChild && JqState.m_Jobs2[nIndex].nFinishedHandle == JqState.m_Jobs2[nIndex].nStartedHandle)
	{
		uint16_t nParent = JqState.m_Jobs2[nIndex].nParent;
		if(nParent)
		{
			uint64_t nParentHandle = JqDetachChild(nJob);
			JqState.m_Jobs2[nIndex].nParent = 0;
			JqCheckFinished(nParentHandle);
		}
		//JqState.m_Jobs2[nIndex].nFinishedHandle = JqState.m_Jobs2[nIndex].nStartedHandle;
		JQ_CLEAR_FUNCTION(JqState.m_Jobs2[nIndex].Function);
		JqState.m_Jobs2[nIndex].PipeHandle.store(JqPipeHandleNull());
		JqState.Stats.nNumFinished++;
		// //kick waiting threads.
		// int8_t nWaiters = JqState.m_Jobs2[nIndex].nWaiters;
		// if(nWaiters != 0)
		// {
		// 	JqState.Stats.nNumWaitKicks++;
		// 	JqState.WaitCond.NotifyAll();
		// 	JqState.m_Jobs2[nIndex].nWaiters = 0;
		// 	JqState.m_Jobs2[nIndex].nWaitersWas = nWaiters;
		// }
		// else
		// {
		// 	JqState.m_Jobs2[nIndex].nWaitersWas = 0xff;
		// }
		JqState.nFreeJobs++;
	}
}

//maybe be called multiple times, but not from multiple threads
//will only be called 
void JqFinishJobHelper(JqPipeHandle PipeHandle, uint32_t nExternalId)
{
	JqMutexLock L(JqState.Mutex);
	JQ_ASSERT(nExternalId < JQ_TREE_BUFFER_SIZE2);
	if(JqState.m_Jobs2[nExternalId].nFinishedHandle != JqState.m_Jobs2[nExternalId].nStartedHandle)
	{
		JQ_ASSERT(JqState.m_Jobs2[nExternalId].PipeHandle.load().Handle == PipeHandle.Handle);
		uint64_t nHandle = JqState.m_Jobs2[nExternalId].nStartedHandle;
		JqState.m_Jobs2[nExternalId].nFinishedHandle = nHandle;
		JqCheckFinished(nHandle);
	}
}


void JqRunJobHelper(JqPipeHandle PipeHandle, uint32_t nExternalId, uint16_t nSubIndex, int nNumJobs, int nRange)
{
	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JQ_ASSERT(nExternalId < JQ_TREE_BUFFER_SIZE2);
	while(JqState.m_Jobs2[nExternalId].PipeHandle.load(std::memory_order_acquire).Handle != PipeHandle.Handle)	//spin untill external id is actually set
		;
	uint64_t nHandle = JqState.m_Jobs2[nExternalId].nStartedHandle;
	JqSelfPush(nHandle, nSubIndex);
	//uint32_t nHandleInternal = PipeHandle.nHandleInternal;
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;	
	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
#ifdef JQ_NO_LAMBDA
	JqState.m_Jobs2[nExternalId].Function(JqState.m_Jobs2[nExternalId].pArg, nStart, nEnd);
#else
	JqState.m_Jobs2[nExternalId].Function(nStart, nEnd);
#endif
	JqSelfPop(nHandle);
}

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
	JQ_ASSERT(nPipeConfigSize == 0 || nPipeConfigSize == JQ_NUM_PIPES * nNumWorkers); //either full spec or nothing
	JQ_ASSERT_NOT_LOCKED();

	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&(uint64_t)&JqState) == 0);
	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&offsetof(JqState_t, Semaphore)) == 0);
	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.nNumWorkers == 0);
	memset(JqState.m_PipeList, 0xff, sizeof(JqState.m_PipeList));
	memset(JqState.m_nNumPipes, 0, sizeof(JqState.m_nNumPipes));
	memset(JqState.m_Pipes, 0, sizeof(JqState.m_Pipes));
	memset(JqState.m_Jobs2, 0, sizeof(JqState.m_Jobs2));

	for(uint32_t i = 0; i < nNumWorkers; ++i)
	{
		uint8_t* pPipes = pPipeConfig + JQ_NUM_PIPES * i;
		uint8_t nNumActivePipes = 0;
		if(nPipeConfigSize)
		{
			for(uint32_t j = 0; j <JQ_NUM_PIPES; ++j)
			{
				if(pPipes[j] != 0xff)
				{
					JqState.m_PipeList[i][nNumActivePipes++] = pPipes[j];
				}
			}
		}
		else
		{
			for(uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
			{
				JqState.m_PipeList[i][nNumActivePipes++] = j;
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.m_nNumPipes[i] = nNumActivePipes;
	}
	for(uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
	{
		JqState.m_Pipes[i].nPut.store(1);
		JqState.m_Pipes[i].nGet.store(1);
		JqState.m_Pipes[i].StartJobFunc = JqRunJobHelper;
		JqState.m_Pipes[i].FinishJobFunc = JqFinishJobHelper;
		JqState.m_Pipes[i].nPipeId = (uint8_t)i;
	}
	JqState.Semaphore.Init(nNumWorkers);
	JqState.nTotalWaiting = 0;
	JqState.nNumWorkers = nNumWorkers;
	JqState.nStop = 0;
	JqState.nStopSentinel = 0;
	JqState.nSentinelRunning = 0;
	JqState.nTotalWaiting = 0;
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
	JqState.Semaphore.Signal(JqState.nNumWorkers);
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JQ_THREAD_JOIN(&JqState.WorkerThreads[i]);
		JQ_THREAD_DESTROY(&JqState.WorkerThreads[i]);
	}
	if(JqState.nSentinelRunning)
	{
		JqState.nStopSentinel = 1;
		JQ_THREAD_JOIN(&JqState.SentinelThread);
		JQ_THREAD_DESTROY(&JqState.SentinelThread);


	}
	JqState.nNumWorkers = 0;
}

void JqConsumeStats(JqStats* pStats)
{
	JqMutexLock lock(JqState.Mutex);
	*pStats = JqState.Stats;
	JqState.Stats.nNumAdded = 0;
	JqState.Stats.nNumFinished = 0;
	JqState.Stats.nNumAddedSub = 0;
	JqState.Stats.nNumFinishedSub = 0;
	JqState.Stats.nNumLocks = 0;
	JqState.Stats.nNumWaitKicks = 0;
	JqState.Stats.nNumWaitCond = 0;
}




// uint16_t JqIncrementStarted(uint64_t nJob)
// {
// 	JQ_ASSERT_LOCKED();
// 	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2; 
// 	JQ_ASSERT(JqState.m_Jobs2[nIndex].nNumJobs > JqState.m_Jobs2[nIndex].nNumStarted);
// 	uint16_t nSubIndex = JqState.m_Jobs2[nIndex].nNumStarted++;
// 	if(JqState.m_Jobs2[nIndex].nNumStarted == JqState.m_Jobs2[nIndex].nNumJobs)
// 	{
// 		JqPriorityListRemove(nIndex);
// 	}
// 	return nSubIndex;
// }
// void JqIncrementFinished(uint64_t nJob)
// {
// 	JQ_ASSERT_LOCKED();
// 	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
// 	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2; 
// 	JqState.m_Jobs2[nIndex].nNumFinished++;
// 	JqState.Stats.nNumFinishedSub++;
// 	JqCheckFinished(nJob);
// }


void JqAttachChild(uint64_t nParent, uint64_t nChild)
{
	JQ_ASSERT_LOCKED();
	uint16_t nParentIndex = nParent % JQ_TREE_BUFFER_SIZE2;
	uint16_t nChildIndex = nChild % JQ_TREE_BUFFER_SIZE2;
	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].nStartedHandle == nChild);
	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].nFinishedHandle != nChild);
	JQ_ASSERT(JqState.m_Jobs2[nParentIndex].nStartedHandle == nParent);
	JQ_ASSERT(JqState.m_Jobs2[nParentIndex].nFinishedHandle != nParent);
	uint16_t nFirstChild = JqState.m_Jobs2[nParentIndex].nFirstChild;
	JqState.m_Jobs2[nChildIndex].nParent = nParentIndex;
	JqState.m_Jobs2[nChildIndex].nSibling = nFirstChild;
	JqState.m_Jobs2[nChildIndex].nRoot = JqState.m_Jobs2[nParentIndex].nRoot;
	JqState.m_Jobs2[nParentIndex].nFirstChild = nChildIndex;


}

uint64_t JqDetachChild(uint64_t nChild)
{
	uint16_t nChildIndex = nChild % JQ_TREE_BUFFER_SIZE2;
	uint16_t nParentIndex = JqState.m_Jobs2[nChildIndex].nParent;
	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].nStartedHandle == JqState.m_Jobs2[nChildIndex].nFinishedHandle);
	if(0 == nParentIndex)
	{
		return 0;
	}
	JQ_ASSERT(nParentIndex != 0);
	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].nFirstChild == 0);
	uint16_t* pChildIndex = &JqState.m_Jobs2[nParentIndex].nFirstChild;
	JQ_ASSERT(JqState.m_Jobs2[nParentIndex].nFirstChild != 0); 
	while(*pChildIndex != nChildIndex)
	{
		JQ_ASSERT(JqState.m_Jobs2[*pChildIndex].nSibling != 0);
		pChildIndex = &JqState.m_Jobs2[*pChildIndex].nSibling;

	}
	JQ_ASSERT(pChildIndex);
	JqState.m_Jobs2[nChildIndex].nRoot = 0;
	*pChildIndex = JqState.m_Jobs2[nChildIndex].nSibling;
	return JqState.m_Jobs2[nParentIndex].nStartedHandle;
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

void JqSelfPush(uint64_t nHandle, uint32_t nSubIndex)
{	
	JqSelfStack[JqSelfPos].nHandle = nHandle;
	JqSelfStack[JqSelfPos].nJobIndex = nSubIndex;
	JqSelfPos++;
}

void JqSelfPop(uint64_t nHandle)
{
	JQ_ASSERT(JqSelfPos != 0);
	JqSelfPos--;
	JQ_ASSERT(JqSelfStack[JqSelfPos].nHandle == nHandle);	
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

// void JqExecuteJob(uint64_t nJob, uint16_t nSubIndex)
// {
// 	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
// 	JQ_ASSERT_NOT_LOCKED();
// 	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
// 	JqSelfPush(nJob, nSubIndex);
// 	uint16_t nWorkIndex = nJob % JQ_TREE_BUFFER_SIZE2;
// 	uint16_t nNumJobs = JqState.m_Jobs2[nWorkIndex].nNumJobs;
// 	int nRange = JqState.m_Jobs2[nWorkIndex].nRange;
// 	int nFraction = nRange / nNumJobs;
// 	int nRemainder = nRange - nFraction * nNumJobs;	
// 	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
// 	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
// #ifdef JQ_NO_LAMBDA
// 	JqState.m_Jobs2[nWorkIndex].Function(JqState.m_Jobs2[nWorkIndex].pArg, nStart, nEnd);
// #else
// 	JqState.m_Jobs2[nWorkIndex].Function(nStart, nEnd);
// #endif
// 	JqSelfPop(nJob);
// 	//JqIncrementFinished(nJob);
// }


// uint16_t JqTakeJob(uint16_t* pSubIndex, int nShortOnly)
// {
// 	JQ_ASSERT_LOCKED();
// 	if(nShortOnly)
// 	{
// 		for(int i = 0; i < JQ_PRIORITY_ARRAY_SIZE; i+=2)
// 		{
// 			uint16_t nIndex = JqState.nPrioListHead[i];
// 			if(nIndex)
// 			{
// 				*pSubIndex = JqIncrementStarted(JqState.m_Jobs2[nIndex].nStartedHandle);
// 				return nIndex;
// 			}
// 		}
// 	}
// 	else
// 	{
// 		for(int i = 0; i < JQ_PRIORITY_ARRAY_SIZE; ++i)
// 		{
// 			uint16_t nIndex = JqState.nPrioListHead[i];
// 			if(nIndex)
// 			{
// 				*pSubIndex = JqIncrementStarted(JqState.m_Jobs2[nIndex].nStartedHandle);
// 				return nIndex;
// 			}
// 		}
// 	}
// 	return 0;
// }
#ifdef JQ_ASSERT_SANITY
// void JqTagChildren(uint16_t nRoot)
// {
// 	for(int i = 1; i < JQ_TREE_BUFFER_SIZE2; ++i)
// 	{
// 		JQ_ASSERT(JqState.m_Jobs2[i].nTag == 0);
// 		if(nRoot == i)
// 		{
// 			JqState.m_Jobs2[i].nTag = 1;
// 		}
// 		else
// 		{
// 			int nParent = JqState.m_Jobs2[i].nParent;
// 			while(nParent)
// 			{
// 				if(nParent == nRoot)
// 				{
// 					JqState.m_Jobs2[i].nTag = 1;
// 					break;
// 				}
// 				nParent = JqState.m_Jobs2[nParent].nParent;
// 			}
// 		}
// 	}

// }
// void JqCheckTagChildren(uint16_t nRoot)
// {
// 	for(int i = 1; i < JQ_TREE_BUFFER_SIZE2; ++i)
// 	{
// 		JQ_ASSERT(JqState.m_Jobs2[i].nTag == 0);
// 		bool bTagged = false;
// 		if(nRoot == i)
// 			bTagged = false;
// 		else
// 		{
// 			int nParent = JqState.m_Jobs2[i].nParent;
// 			while(nParent)
// 			{
// 				if(nParent == nRoot)
// 				{
// 					bTagged = true;
// 					break;
// 				}
// 				nParent = JqState.m_Jobs2[nParent].nParent;
// 			}
// 		}
// 		JQ_ASSERT(bTagged == (1==JqState.m_Jobs2[i].nTag));
// 	}
// }

// void JqLoopChildren(uint16_t nRoot)
// {
// 	int nNext = JqState.m_Jobs2[nRoot].nFirstChild;
// 	while(nNext != nRoot && nNext)
// 	{
// 		while(JqState.m_Jobs2[nNext].nFirstChild)
// 			nNext = JqState.m_Jobs2[nNext].nFirstChild;
// 		JQ_ASSERT(JqState.m_Jobs2[nNext].nTag == 1);
// 		JqState.m_Jobs2[nNext].nTag = 0;
// 		if(JqState.m_Jobs2[nNext].nSibling)
// 			nNext = JqState.m_Jobs2[nNext].nSibling;
// 		else
// 		{
// 			//search up
// 			nNext = JqState.m_Jobs2[nNext].nParent;
// 			while(nNext != nRoot)
// 			{
// 				JQ_ASSERT(JqState.m_Jobs2[nNext].nTag == 1);
// 				JqState.m_Jobs2[nNext].nTag = 0;
// 				if(JqState.m_Jobs2[nNext].nSibling)
// 				{
// 					nNext = JqState.m_Jobs2[nNext].nSibling;
// 					break;
// 				}
// 				else
// 				{
// 					nNext = JqState.m_Jobs2[nNext].nParent;
// 				}

// 			}
// 		}
// 	}

// 	JQ_ASSERT(JqState.m_Jobs2[nRoot].nTag == 1);
// 	JqState.m_Jobs2[nRoot].nTag = 0;
// }
#endif


//Depth first. Once a node is visited all child nodes have been visited
uint16_t JqTreeIterate(uint64_t nJob, uint16_t nCurrent)
{
	JQ_ASSERT(nJob);
	uint16_t nRoot = nJob % JQ_TREE_BUFFER_SIZE2;
	if(JqState.m_Jobs2[nRoot].nRoot != JqState.m_Jobs2[nCurrent].nRoot)
	{
		nCurrent = nRoot; //not a child. restart iteration.
	}
	if(nRoot == nCurrent)
	{
		while(JqState.m_Jobs2[nCurrent].nFirstChild)
			nCurrent = JqState.m_Jobs2[nCurrent].nFirstChild;
	}
	else
	{
		//once here all child nodes _have_ been processed.
		if(JqState.m_Jobs2[nCurrent].nSibling)
		{
			nCurrent = JqState.m_Jobs2[nCurrent].nSibling;
			while(JqState.m_Jobs2[nCurrent].nFirstChild) //child nodes first.
				nCurrent = JqState.m_Jobs2[nCurrent].nFirstChild;
		}
		else
		{
			nCurrent = JqState.m_Jobs2[nCurrent].nParent;
		}
	}
	return nCurrent;
}


// //this code is O(n) where n is the no. of child nodes.
// //I wish this could be written in a simpler way
// uint16_t JqTakeChildJob(uint64_t nJob, uint16_t* pSubIndexOut)
// {
// 	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeChildJob", 0xff);
// 	JQ_ASSERT_LOCKED();
// #if JQ_ASSERT_SANITY
// 	{
// 		//verify that the child iteration is sane
// 		for(int i = 0; i < JQ_TREE_BUFFER_SIZE2; ++i)
// 		{
// 			JqState.m_Jobs2[i].nTag = 0;
// 		}
// 		JqTagChildren(nJob%JQ_TREE_BUFFER_SIZE2);
// 		uint16_t nIndex = nJob%JQ_TREE_BUFFER_SIZE2;
// 		uint16_t nRootIndex = nJob % JQ_TREE_BUFFER_SIZE2;
// 		do
// 		{
// 			nIndex = JqTreeIterate(nJob, nIndex);
// 			JQ_ASSERT(JqState.m_Jobs2[nIndex].nTag == 1);
// 			JqState.m_Jobs2[nIndex].nTag = 0;
// 		}while(nIndex != nRootIndex);
// 		for(int i = 0; i < JQ_TREE_BUFFER_SIZE2; ++i)
// 		{
// 			JQ_ASSERT(JqState.m_Jobs2[i].nTag == 0);
// 		}
// 	}
// #endif

// 	uint16_t nRoot = nJob % JQ_TREE_BUFFER_SIZE2;
// 	uint16_t nIndex = nRoot;

// 	do
// 	{
// 		nIndex = JqTreeIterate(nJob, nIndex);
// 		JQ_ASSERT(nIndex);

// 		if(JqState.m_Jobs2[nIndex].nNumStarted < JqState.m_Jobs2[nIndex].nNumJobs)
// 		{
// 			*pSubIndexOut = JqIncrementStarted(JqState.m_Jobs2[nIndex].nStartedHandle);
// 			return nIndex;
// 		}
// 	}while(nIndex != nRoot);

// 	return 0;
// }

bool JqExecuteOne(uint8_t* pPipes, uint8_t nNumPipes);
bool JqExecuteOne()
{
	return JqExecuteOne(g_JqPipes, g_nJqNumPipes);
}

bool JqExecuteOne(uint8_t* pPipes, uint8_t nNumPipes)
{
	if(0 == nNumPipes)
	{
		for(uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
		{
			JqPipeHandle Empty;
			Empty.Handle = 0;
			EJqPipeExecuteResult eRes = JqPipeExecute(&JqState.m_Pipes[i], Empty);
			if(eRes != EJQ_EXECUTE_FAIL)
				return true;
		}
	}
	else
	{
		for(uint32_t i = 0; i < nNumPipes; ++i)
		{
			JqPipeHandle Empty;
			Empty.Handle = 0;
			EJqPipeExecuteResult eRes = JqPipeExecute(&JqState.m_Pipes[pPipes[i]], Empty);
			if(eRes != EJQ_EXECUTE_FAIL)
				return true;
		}		
	}
	return false;
}

bool JqExecuteOneChild(uint64_t nJob)
{
	if(JqIsDone(nJob))
		return false;
	JqMutexLock Lock(JqState.Mutex);


	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint16_t nRoot = nJob % JQ_TREE_BUFFER_SIZE2;

	do
	{
		nIndex = JqTreeIterate(nJob, nIndex);
		JQ_ASSERT(nIndex);
		JqPipeHandle ChildHandle = JqState.m_Jobs2[nIndex].PipeHandle.load();
		Lock.Unlock();
		if(JqPipeExecute(&JqState.m_Pipes[ChildHandle.Pipe], ChildHandle))
		{
			return true;
		}
		Lock.Lock();
	}while(nIndex != nRoot);
	return false;
}





void JqWorker(int nThreadId)
{
	uint8_t* pPipes = JqState.m_PipeList[nThreadId];
	uint32_t nNumPipes = JqState.m_nNumPipes[nThreadId];
	char PipeStr[JQ_NUM_PIPES+1];
#ifdef JQ_MICROPROFILE
	memset(PipeStr, '0', sizeof(PipeStr)-1);
	PipeStr[JQ_NUM_PIPES] = '\0';
	for(uint32_t i = 0; i < nNumPipes; ++i)
	{
		if(pPipes[i] < JQ_NUM_PIPES)
		{
			PipeStr[pPipes[i]] = '1';
		}
	}
	char sWorker[64];
	snprintf(sWorker, sizeof(sWorker)-1, "JqWorker %d %s", nThreadId, PipeStr);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif


	while(0 == JqState.nStop)
	{
		while(JqState.nFrozen.load())
		{
			JQ_USLEEP(1000);
		}
		bool bExecuted = false;
		for(uint32_t i = 0; i < nNumPipes; ++i)
		{
			if(JqExecuteOne(pPipes, nNumPipes))
			{
				bExecuted = true;
				break;
			}
		}
		if(!bExecuted)
		{
			JqState.Semaphore.Wait();
		}
	}
#ifdef JQ_MICROPROFILE
	MicroProfileOnThreadExit();
#endif
}

void JqDump(uint32_t nNode, uint32_t nIndent)
{
	JqJob2* pJob = &JqState.m_Jobs2[nNode];
	for(uint32_t i = 0; i < nIndent; ++i )
		printf("  ");
	printf("%08d %lld/%lld, root %lld pipe %d, pipehandle %d\n", nNode, pJob->nStartedHandle, pJob->nFinishedHandle, pJob->nRoot, pJob->PipeHandle.load().Pipe, pJob->PipeHandle.load().HandleInt);
}

void JqDumpTree(uint32_t nTree, uint32_t nDepth)
{
	JqDump(nTree, nDepth);
	JqJob2* pJob = &JqState.m_Jobs2[nTree];
	uint16_t nChild = pJob->nFirstChild;
	while(nChild)
	{
		JqDumpTree(nChild, nDepth + 1);
		nChild = JqState.m_Jobs2[nChild].nSibling;
	}
}

void JqCrashAndDump()
{
	JqState.nFrozen.store(1);
	JQ_USLEEP(1000*1000);

	printf("JQ CRASHING\n");
	uint32_t nPendingJobs = 0;
	for(uint32_t i = 1; i < JQ_TREE_BUFFER_SIZE2; ++i)
	{
		JqJob2* pJob = &JqState.m_Jobs2[i];
		if(pJob->nStartedHandle != pJob->nFinishedHandle)
		{
			if(0 == pJob->nParent)
			{
				printf("root %d\n", i);
				JqDumpTree(i, 0);
			}
			nPendingJobs++;
		}
	}
	printf("total pending jobs %d/%d\n", nPendingJobs, JQ_TREE_BUFFER_SIZE2);
	printf("Dumping pipes\n");
	for(uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
	{
		JqPipeDump(&JqState.m_Pipes[i]);
	}

	JQ_ASSERT(0);

}

void JqSentinel(int nSeconds)
{
	int64_t nTickLimit = JqTicksPerSecond() * nSeconds;
	int64_t nTicks = 0;
	int64_t nTickLast = JqTick();
	uint64_t nJobs = JqState.nJobsFinished.load();
	while(0 == JqState.nStopSentinel)
	{
		JQ_USLEEP(50000);
		int64_t nJobsCur = JqState.nJobsFinished.load();
		if(nJobsCur != nJobs)
		{
			nTicks = 0;
		}
		else
		{
			nTicks += JqTick() - nTickLast;
		}
		if(nTicks > nTickLimit)
		{
			JqCrashAndDump();
		}
		nTickLast = JqTick();

	}
}
void JqStartSentinel(int nTimeout)
{
	JQ_ASSERT(!JqState.nSentinelRunning);
	JqState.nSentinelRunning = 1;
	JQ_THREAD_CREATE(&JqState.SentinelThread);
	JQ_THREAD_START(&JqState.SentinelThread, JqSentinel, nTimeout);
}
// uint64_t JqNextHandle(uint64_t nJob)
// {
// 	nJob++;
// 	if(0 == (nJob%JQ_TREE_BUFFER_SIZE2))
// 	{
// 		nJob++;
// 	}
// 	return nJob;
// }
// uint64_t JqFindHandle(JqMutexLock& Lock)
// {
// 	JQ_ASSERT_LOCKED();
// 	while(!JqState.nFreeJobs)
// 	{
// 		if(JqSelfPos < JQ_MAX_JOB_STACK)
// 		{
// 			JQ_MICROPROFILE_SCOPE("AddExecute", 0xff); // if this starts happening the job queue size should be increased..
// 			uint16_t nSubIndex = 0;
// 			uint16_t nIndex = JqTakeJob(&nSubIndex, 0);
// 			if(nIndex)
// 			{
// 				Lock.Unlock();
// 				JqExecuteJob(JqState.m_Jobs2[nIndex].nStartedHandle, nSubIndex);
// 				Lock.Lock();
// 				JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);
// 			}
// 		}
// 		else
// 		{
// 			JQ_BREAK(); //out of job queue space. increase JQ_TREE_BUFFER_SIZE2 or create fewer jobs
// 		}
// 	}
// 	JQ_ASSERT(JqState.nFreeJobs != 0);
// 	uint64_t nNextHandle = JqState.nNextHandle;
// 	uint16_t nCount = 0;
// 	while(JqPendingJobs(nNextHandle))
// 	{
// 		nNextHandle = JqNextHandle(nNextHandle);
// 		JQ_ASSERT(nCount++ < JQ_TREE_BUFFER_SIZE2);
// 	}
// 	JqState.nNextHandle = JqNextHandle(nNextHandle);
// 	JqState.nFreeJobs--;	
// 	return nNextHandle;
// }

#ifdef JQ_NO_LAMBDA
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs, int nRange, uint32_t nWaitFlag)
#else
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nWaitFlag)
#endif
{
#ifdef JQ_NO_LAMBDA
	uint64_t nJob = JqAdd(JobFunc, nPrio, pArg, nNumJobs, nRange);
#else
	uint64_t nJob = JqAdd(JobFunc, nPrio, nNumJobs, nRange);
#endif
	JqWait(nJob, nWaitFlag);
}

uint64_t JqNextHandle()
{
	uint64_t nHandle = JqState.nNextHandle++;//.fetch_add(1);
	uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
	while(nIndex == 0 || JqState.m_Jobs2[nIndex].nStartedHandle != JqState.m_Jobs2[nIndex].nFinishedHandle)
	{
		nHandle = JqState.nNextHandle++;
		nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
	}

	return nHandle;
}

#ifdef JQ_NO_LAMBDA
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPipe, void* pArg, int nNumJobs, int nRange, uint64_t nParent)
#else
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPipe, int nNumJobs, int nRange, uint64_t nParent)
#endif
{
	JQ_ASSERT(nPipe < JQ_NUM_PIPES);
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
	uint64_t nHandle;
	{
		JqMutexLock Lock(JqState.Mutex);
		nHandle = JqNextHandle();
		uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
		JqJob2* pEntry = &JqState.m_Jobs2[nIndex];
		JQ_ASSERT(pEntry->PipeHandle.load().Handle == 0);
		//pEntry->PipeHandle.store(JqPipeHandleNull(), std::memory_order_release);
		pEntry->Function = JobFunc;
#ifdef JQ_NO_LAMBDA
		pEntry->pArg = pArg;
#endif
		pEntry->nStartedHandle = nHandle;

		uint64_t nParentHandle = nParent;
		pEntry->nParent = nParentHandle % JQ_TREE_BUFFER_SIZE2;
		if(pEntry->nParent)
		{

			JqAttachChild(nParentHandle, nHandle);
		}
		else
		{
			pEntry->nRoot = nHandle;
		}
		JqPipeHandle Handle = JqPipeAdd(&JqState.m_Pipes[nPipe], (uint32_t)nHandle, nNumJobs, nRange);
		pEntry->PipeHandle.store(Handle, std::memory_order_release);
		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += nNumJobs;
	}

	JqState.Semaphore.Signal(nNumJobs);
	return nHandle;
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint64_t nFinishedHandle = JqState.m_Jobs2[nIndex].nFinishedHandle;
	uint64_t nStartedHandle = JqState.m_Jobs2[nIndex].nStartedHandle;
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
		uint64_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
		JqPipeHandle PipeHandle = JqState.m_Jobs2[nIndex].PipeHandle.load();
		return JqPipeIsDone(&JqState.m_Pipes[PipeHandle.Pipe], PipeHandle);
	}
	return bIsDone;
}


// bool JqPendingJobs(uint64_t nJob)
// {
// 	uint64_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
// 	JQ_ASSERT(JQ_LE_WRAP(JqState.m_Jobs2[nIndex].nFinishedHandle, JqState.m_Jobs2[nIndex].nStartedHandle));
// 	return JqState.m_Jobs2[nIndex].nFinishedHandle != JqState.m_Jobs2[nIndex].nStartedHandle;
// }


void JqWaitAll()
{
	while(JqExecuteOne(nullptr, 0))
		;
	return;
	// uint16_t nIndex = 0;
	// while(JqState.nFreeJobs != JQ_NUM_JOBS)
	// {
	// 	uint16_t nSubIndex = 0;
	// 	{
	// 		JqMutexLock lock(JqState.Mutex);
	// 		if(nIndex)
	// 		{
	// 			JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);		
	// 		}
	// 		nIndex = JqTakeJob(&nSubIndex, 0);
	// 	}
	// 	if(nIndex)
	// 	{
	// 		JqExecuteJob(JqState.m_Jobs2[nIndex].nStartedHandle, nSubIndex);
	// 	}
	// 	else
	// 	{
	// 		JQ_USLEEP(1000);
	// 	}	
	// }
	// if(nIndex)
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);		
	// }
}


void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	//uint16_t nIndex = 0;
	if(JqIsDone(nJob))
	{
		return;
	}
	while(!JqIsDoneExt(nJob, nWaitFlag))
	{
		bool bExecuted = false;

		// uint16_t nSubIndex = 0;
		if((nWaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			bExecuted = JqExecuteOneChild(nJob);
			if(!bExecuted)
			{
				bExecuted = JqExecuteOne();
			}
			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);
			// if(JqIsDoneExt(nJob, nWaitFlag)) 
			// 	return;
			// nIndex = JqTakeChildJob(nJob, &nSubIndex);
			// if(!nIndex)
			// {
			// 	nIndex = JqTakeJob(&nSubIndex, 0);
			// }
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			bExecuted = JqExecuteOneChild(nJob);

			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);
			// if(JqIsDoneExt(nJob, nWaitFlag)) 
			// 	return;
			// nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		else if(0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			bExecuted = JqExecuteOne();

			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);
			// if(JqIsDoneExt(nJob, nWaitFlag)) 
			// 	return;
			// nIndex = JqTakeJob(&nSubIndex, 0);
		}
		else
		{
			JQ_BREAK();
		}
		if(!bExecuted)
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
					JqSpinloop |= result; //write it somewhere so the optimizer can't remove it
				}while( (1000000ull*(JqTick()-nTick)) / nTicksPerSecond < nUsWaitTime);


			}
			else if(nWaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(nUsWaitTime);
			}
			else
			{
				JQ_ASSERT(0); // no longer supported
				// JqMutexLock lock(JqState.Mutex);
				// if(JqIsDoneExt(nJob, nWaitFlag))
				// {
				// 	return;
				// }
				// uint16_t nJobIndex = nJob % JQ_TREE_BUFFER_SIZE2;
				// JqState.m_Jobs2[nJobIndex].nWaiters++;
				// JqState.Stats.nNumWaitCond++;
				// JqState.WaitCond.Wait(JqState.Mutex);
				// JqState.m_Jobs2[nJobIndex].nWaiters--;
			}
		}
		// else
		// {
		// 	JqExecuteJob(JqState.m_Jobs2[nIndex].nStartedHandle, nSubIndex);
		// }
	}
	// if(nIndex)
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);
	// }

}


void JqExecuteChildren(uint64_t nJob)
{
	JQ_ASSERT(0);
	// if(JqIsDone(nJob))
	// {
	// 	return;
	// }

	// uint16_t nIndex = 0;
	// do 
	// {		
	// 	uint16_t nSubIndex = 0;
	// 	{
	// 		JqMutexLock lock(JqState.Mutex);
	// 		if(nIndex)
	// 			JqIncrementFinished(JqState.m_Jobs2[nIndex].nStartedHandle);

	// 		if(JqIsDone(nJob)) 
	// 			return;

	// 		nIndex = JqTakeChildJob(nJob, &nSubIndex);
	// 	}
	// 	if(nIndex)
	// 	{
	// 		JqExecuteJob(JqState.m_Jobs2[nIndex].nStartedHandle, nSubIndex);
	// 	}

	// }while(nIndex);
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

	uint64_t nHandle = JqNextHandle();
	uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
	JqJob2* pEntry = &JqState.m_Jobs2[nIndex];
	pEntry->PipeHandle.store(JqPipeHandleNull(), std::memory_order_release);
	JQ_ASSERT(JQ_LE_WRAP(pEntry->nFinishedHandle, nHandle));
	pEntry->nStartedHandle = nHandle;

	uint64_t nParentHandle = JqSelf();
	pEntry->nParent = nParentHandle % JQ_TREE_BUFFER_SIZE2;
	if(pEntry->nParent)
	{
		JqAttachChild(nParentHandle, nHandle);
	}
	else
	{
		pEntry->nRoot = nHandle;
	}
	JQ_CLEAR_FUNCTION(pEntry->Function);
#ifdef JQ_NO_LAMBDA
	pEntry->pArg = 0;
#endif
	JqSelfPush(nHandle, 0);
	return nHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);

	JqMutexLock lock(JqState.Mutex);
	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint64_t nHandle = JqState.m_Jobs2[nIndex].nStartedHandle;
	JqState.m_Jobs2[nIndex].nFinishedHandle = nHandle;
	JqCheckFinished(nHandle);
}

uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1].nHandle : 0;
}


// void JqPriorityListAdd(uint16_t nIndex)
// {
// 	uint8_t nPrio = JqState.m_Jobs2[nIndex].nPrio;
// 	JQ_ASSERT(JqState.m_Jobs2[nIndex].nLinkNext == 0);
// 	JQ_ASSERT(JqState.m_Jobs2[nIndex].nLinkPrev == 0);
// 	uint16_t nTail = JqState.nPrioListTail[nPrio];
// 	if(nTail != 0)
// 	{
// 		JQ_ASSERT(JqState.m_Jobs2[nTail].nLinkNext == 0);
// 		JqState.m_Jobs2[nTail].nLinkNext = nIndex;
// 		JqState.m_Jobs2[nIndex].nLinkPrev = nTail;
// 		JqState.m_Jobs2[nIndex].nLinkNext = 0;
// 		JqState.nPrioListTail[nPrio] = nIndex;
// 	}
// 	else
// 	{
// 		JQ_ASSERT(JqState.nPrioListHead[nPrio] == 0);
// 		JqState.nPrioListHead[nPrio] = nIndex;
// 		JqState.nPrioListTail[nPrio] = nIndex;
// 		JqState.m_Jobs2[nIndex].nLinkNext = 0;
// 		JqState.m_Jobs2[nIndex].nLinkPrev = 0;
// 	}
// }
// void JqPriorityListRemove(uint16_t nIndex)
// {
// 	uint8_t nPrio = JqState.m_Jobs2[nIndex].nPrio;
// 	uint16_t nNext = JqState.m_Jobs2[nIndex].nLinkNext;
// 	uint16_t nPrev = JqState.m_Jobs2[nIndex].nLinkPrev;
// 	JqState.m_Jobs2[nIndex].nLinkNext = 0;
// 	JqState.m_Jobs2[nIndex].nLinkPrev = 0;

// 	if(nNext != 0)
// 	{
// 		JqState.m_Jobs2[nNext].nLinkPrev = nPrev;
// 	}
// 	else
// 	{
// 		JQ_ASSERT(JqState.nPrioListTail[nPrio] == nIndex);
// 		JqState.nPrioListTail[nPrio] = nPrev;
// 	}
// 	if(nPrev != 0)
// 	{
// 		JqState.m_Jobs2[nPrev].nLinkNext = nNext;
// 	}
// 	else
// 	{
// 		JQ_ASSERT(JqState.nPrioListHead[nPrio] == nIndex);
// 		JqState.nPrioListHead[nPrio] = nNext;
// 	}

// }

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
