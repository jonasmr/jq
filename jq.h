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
//		lock fewer times per op
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
#define JQ_WORK_BUFFER_SIZE (1024)
#endif

#ifndef JQ_PRIORITY_MAX
#define JQ_PRIORITY_MAX 7
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
#define JQ_FUNCTION_SIZE 64
#endif

#define JQ_ASSERT_SANITY 0



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
		static_assert(std::is_trivial<F>::value, "Only captures of trivial types supported. Use std::function if you think you need non-trivial types");
		static_assert(sizeof(F) <= JQ_FUNCTION_SIZE, "Captured lambda is too big. Increase size or capture less");
		static_assert(alignof(F) <= alignof(void*), "Alignment requirements too high");
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
#define JQ_WAITFLAG_SPIN 0x4
#define JQ_WAITFLAG_SLEEP 0x8

#ifdef JQ_NO_LAMBDA
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs = 1, int nRange = -1);
#else
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1);
#endif
JQ_API void 		JqWait(uint64_t nJob, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SLEEP, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void 		JqWaitAll(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SLEEP, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API uint64_t		JqWaitAny(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SLEEP, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API uint64_t 	JqGroupBegin(); //add a non-executing job to group all jobs added between begin/end
JQ_API void 		JqGroupEnd();
JQ_API bool 		JqIsDone(uint64_t nJob);
JQ_API void 		JqStart(int nNumWorkers);
JQ_API void 		JqStop();



///////////////////////////////////////////////////////////////////////////////////////////
/// Implementation

#ifdef JQ_IMPL
#if defined(__APPLE__)
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
		timeBeginPeriod(1);
		Sleep((DWORD)(usec/1000));
		timeEndPeriod(1);
	}
	else
	{
		Sleep(0);
	}
}
#endif

#include <mutex>
#include <condition_variable>
#include <thread>


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

template<typename T>
struct JqMutexLock;

void 		JqStart(int nNumWorkers);
void 		JqStartop();
void 		JqCheckFinished(uint64_t nJob);
uint16_t 	JqIncrementStarted(uint64_t nJob);
void 		JqIncrementFinished(uint64_t nJob);
void		JqAttachChild(uint64_t nParentJob, uint64_t nChildJob);
uint64_t 	JqDetachChild(uint64_t nChildJob);
void 		JqExecuteJob(uint64_t nJob, uint16_t nJobSubIndex);
uint16_t 	JqTakeJob(uint16_t* pJobSubIndex);
uint16_t 	JqTakeChildJob(uint64_t nJob, uint16_t* pJobSubIndex);
void 		JqWorker(int nThreadId);
uint64_t 	JqNextHandle(uint64_t nJob);
void 		JqWaitAll();
void 		JqPriorityListAdd(uint16_t nJobIndex);
void 		JqPriorityListRemove(uint16_t nJobIndex);
uint64_t 	JqSelf();
bool 		JqPendingJobs(uint64_t nJob);
void 		JqSelfPush(uint64_t nJob);
void 		JqSelfPop(uint64_t nJob);
uint64_t 	JqFindHandle(JqMutexLock<std::mutex>& Lock);



JQ_THREAD_LOCAL uint64_t JqSelfStack[JQ_MAX_JOB_STACK] = {0};
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;
JQ_THREAD_LOCAL uint32_t JqHasLock = 0;


struct JqJob
{
	JqFunction Function;
#ifdef JQ_NO_LAMBDA
	void* pArg;
#endif

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

#ifdef JQ_ASSERT_SANITY
	int nTag;
#endif
};
#ifdef _WIN32
//#define _WIN32_SEMA
#endif

#define JQ_PAD_SIZE(type) (JQ_CACHE_LINE_SIZE - (sizeof(type)%JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))

#include <atomic>

struct JqSemaphore
{
	JqSemaphore();
	~JqSemaphore();
	void Signal(uint32_t nCount);
	void Wait();

#ifdef _WIN32_SEMA
	uint32_t Handle;
#else
	std::mutex Mutex;
	char pad0[JQ_PAD_SIZE(std::mutex)];
	std::condition_variable Cond;
	char pad1[JQ_PAD_SIZE(std::condition_variable)];
	std::atomic<uint32_t> nReleaseCount;

#endif
	uint32_t nNumThreads;

};

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore;
	char pad0[ JQ_PAD_SIZE(JqSemaphore) ]; 
	std::mutex Mutex;
	char pad1[ JQ_PAD_SIZE(std::mutex) ]; 

	std::thread* pWorkerThreads;

	
	int nNumWorkers;
	int nStop;

	uint64_t nNextHandle;
	uint32_t nFreeJobs;

	JqJob Jobs[JQ_WORK_BUFFER_SIZE];
	uint16_t nPrioListHead[JQ_PRIORITY_SIZE];
	uint16_t nPrioListTail[JQ_PRIORITY_SIZE];

	JqState_t()
	:nNumWorkers(0)
	{
	}
} JqState;


template<typename T>
struct JqMutexLock
{
	T& Mutex;
	JqMutexLock(T& Mutex)
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
		Mutex.lock();
		JQ_ASSERT_LOCK_ENTER();
	}
	void Unlock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
		JQ_ASSERT_LOCK_LEAVE();
		Mutex.unlock();
	}
};


void JqStart(int nNumWorkers)
{
	JQ_ASSERT_NOT_LOCKED();

	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&(uint64_t)&JqState) == 0);
	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&offsetof(JqState_t, Semaphore)) == 0);
	JQ_ASSERT( ((JQ_CACHE_LINE_SIZE-1)&offsetof(JqState_t, Mutex)) == 0);

	JQ_ASSERT(JqState.nNumWorkers == 0);
	JqState.pWorkerThreads = new std::thread[nNumWorkers];
	JqState.nNumWorkers = nNumWorkers;
	JqState.nStop = 0;
	for(int i = 0; i < JQ_WORK_BUFFER_SIZE; ++i)
	{
		JqState.Jobs[i].nStartedHandle = 0;
		JqState.Jobs[i].nFinishedHandle = 0;	
	}
	memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.nFreeJobs = JQ_NUM_JOBS;
	JqState.nNextHandle = 1;
	JqState.Semaphore.nNumThreads = JqState.nNumWorkers;

	for(int i = 0; i < nNumWorkers; ++i)
	{
		JqState.pWorkerThreads[i] = std::thread(JqWorker, i);
	}

}

void JqStop()
{
	JqWaitAll();
	JqState.nStop = 1;
	JqState.Semaphore.Signal(JqState.nNumWorkers);
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JqState.pWorkerThreads[i].join();
	}
	delete[] JqState.pWorkerThreads;
	JqState.pWorkerThreads = 0;
	JqState.nNumWorkers = 0;
	JqState.Semaphore.nNumThreads = 0;

}

void JqCheckFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint32_t nIndex = nJob % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(nJob >= JqState.Jobs[nIndex].nFinishedHandle);
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
	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
	JqMutexLock<std::mutex> lock(JqState.Mutex);
	uint16_t nIndex = nJob % JQ_WORK_BUFFER_SIZE; 
	JqState.Jobs[nIndex].nNumFinished++;
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

void JqSelfPush(uint64_t nJob)
{	
	JqSelfStack[JqSelfPos++] = nJob;
}

void JqSelfPop(uint64_t nJob)
{
	JQ_ASSERT(JqSelfPos != 0);
	JqSelfPos--;
	JQ_ASSERT(JqSelfStack[JqSelfPos] == nJob);	
}

void JqExecuteJob(uint64_t nJob, uint16_t nSubIndex)
{
	JQ_MICROPROFILE_SCOPE("ExecuteJob", 0xff);
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JqSelfPush(nJob);
	uint16_t nWorkIndex = nJob % JQ_WORK_BUFFER_SIZE;
	uint16_t nNumJobs = JqState.Jobs[nWorkIndex].nNumJobs;
	int nRange = JqState.Jobs[nWorkIndex].nRange;
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;	
	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
#ifdef JQ_NO_LAMBDA
	JqState.Jobs[nWorkIndex].Function(JqState.Jobs[nWorkIndex].pArg, nStart, nEnd);
#else
	JqState.Jobs[nWorkIndex].Function(nStart, nEnd);
#endif
	JqSelfPop(nJob);
	JqIncrementFinished(nJob);
}


uint16_t JqTakeJob(uint16_t* pSubIndex)
{
	JQ_ASSERT_LOCKED();
	for(int i = 0; i < JQ_PRIORITY_SIZE; ++i)
	{
		uint16_t nIndex = JqState.nPrioListHead[i];
		if(nIndex)
		{
			*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
			return nIndex;
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

void JqWorker(int nThreadId)
{
#ifdef JQ_MICROPROFILE
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker)-1, "JqWorker %d", nThreadId);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.nStop)
	{

		do
		{
			uint16_t nWork = 0;
			uint16_t nSubIndex = 0;
			{
				JqMutexLock<std::mutex> lock(JqState.Mutex);
				nSubIndex = 0;
				nWork = JqTakeJob(&nSubIndex);
			}
			if(!nWork)
			{
				break;
			}
			JqExecuteJob(JqState.Jobs[nWork].nStartedHandle, nSubIndex);
		}while(1);
		JqState.Semaphore.Wait();
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
uint64_t JqFindHandle(JqMutexLock<std::mutex>& Lock)
{
	JQ_ASSERT_LOCKED();
	while(!JqState.nFreeJobs)
	{
		if(JqSelfPos < JQ_MAX_JOB_STACK)
		{
			JQ_MICROPROFILE_SCOPE("AddExecute", 0xff0000); // if this starts happening the job queue size should be increased..
			uint16_t nSubIndex = 0;
			uint16_t nIndex = JqTakeJob(&nSubIndex);
			if(nIndex)
			{
				Lock.Unlock();
				JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
				Lock.Lock();
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

#ifdef JQ_NO_LAMBDA
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs, int nRange)
#else
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange)
#endif
{
	JQ_ASSERT(nPrio < JQ_PRIORITY_SIZE);
	JQ_ASSERT(nNumJobs);
	JQ_ASSERT(JqState.nNumWorkers);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	uint64_t nNextHandle = 0;
	{
		JqMutexLock<std::mutex> Lock(JqState.Mutex);
		nNextHandle = JqFindHandle(Lock);
		uint16_t nIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
		
		JqJob* pEntry = &JqState.Jobs[nIndex];
		JQ_ASSERT(pEntry->nFinishedHandle < nNextHandle);
		pEntry->nStartedHandle = nNextHandle;
		JQ_ASSERT(nNumJobs <= 0xffff);
		pEntry->nNumJobs = (uint16_t)nNumJobs;
		pEntry->nNumStarted = 0;
		pEntry->nNumFinished = 0;
		pEntry->nRange = nRange;
		uint64_t nParentHandle = JqSelf();
		pEntry->nParent = nParentHandle % JQ_WORK_BUFFER_SIZE;
		if(pEntry->nParent)
		{
			JqAttachChild(nParentHandle, nNextHandle);
		}
		pEntry->Function = JobFunc;
		#ifdef JQ_NO_LAMBDA
		pEntry->pArg = pArg;
		#endif
		pEntry->nPrio = nPrio;
		JqPriorityListAdd(nIndex);
	}

	JqState.Semaphore.Signal(nNumJobs);
	return nNextHandle;
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;
	JQ_ASSERT(JqState.Jobs[nIndex].nFinishedHandle <= JqState.Jobs[nIndex].nStartedHandle);
	int64_t nDiff = (int64_t)(JqState.Jobs[nIndex].nFinishedHandle - nJob);
	return nDiff >= 0;
}

bool JqPendingJobs(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_WORK_BUFFER_SIZE;
	JQ_ASSERT(JqState.Jobs[nIndex].nFinishedHandle <= JqState.Jobs[nIndex].nStartedHandle);
	return JqState.Jobs[nIndex].nFinishedHandle != JqState.Jobs[nIndex].nStartedHandle;
}


void JqWaitAll()
{
	while(JqState.nFreeJobs != JQ_NUM_JOBS)
	{
		uint16_t nIndex = 0;
		uint16_t nSubIndex = 0;
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nIndex = JqTakeJob(&nSubIndex);
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
}

JQ_THREAD_LOCAL int JqSpinloop = 0; //prevent optimizer from removing spin loop

void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	while(!JqIsDone(nJob))
	{
		uint16_t nIndex = 0;
		uint16_t nSubIndex = 0;
		if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		
		if(0 == nIndex && 0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nIndex = JqTakeJob(&nSubIndex);
		}
		if(!nIndex)
		{
			JQ_ASSERT(0 != (nWaitFlag & (JQ_WAITFLAG_SLEEP|JQ_WAITFLAG_SPIN)));
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
			else
			{
				JQ_USLEEP(nUsWaitTime);
			}
		}
		else
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}
	}
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
	JqMutexLock<std::mutex> Lock(JqState.Mutex);
	uint64_t nNextHandle = JqFindHandle(Lock);
	uint16_t nIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
	JqJob* pEntry = &JqState.Jobs[nIndex];
	JQ_ASSERT(pEntry->nFinishedHandle < nNextHandle);
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
	#ifdef JQ_NO_LAMBDA
	pEntry->pArg = 0;
	#endif
	pEntry->nPrio = 7;
	JqSelfPush(nNextHandle);
	return nNextHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);

	JqIncrementFinished(nJob);
}



uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1] : 0;
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

#ifdef _WIN32_SEMA
JqSemaphore::JqSemaphore()
{
	Handle = (uint32_t) CreateSemaphore(0, 0, INT_MAX, 0);
	JQ_ASSERT(Handle);
}
JqSemaphore::~JqSemaphore()
{
	CloseHandle((HANDLE)Handle);

}
void JqSemaphore::Signal(uint32_t nCount)
{
	JQ_MICROPROFILE_SCOPE("void JqSemaphore::Signal()", 0xff);	
	BOOL r = ReleaseSemaphore((HANDLE)Handle, nCount, NULL);

}
void JqSemaphore::Wait()
{
	JQ_MICROPROFILE_SCOPE("void JqSemaphore::Wait()", 0xff);
	DWORD r = WaitForSingleObject((HANDLE)Handle, INFINITE);
}
#else
JqSemaphore::JqSemaphore()
{
	nNumThreads = 0xffffffff;
	nReleaseCount = 0;
}
JqSemaphore::~JqSemaphore()
{

}
void JqSemaphore::Signal(uint32_t nCount)
{
	JQ_MICROPROFILE_SCOPE("void JqSemaphore::Signal()", 0xff);	
	uint32_t nCurrent = nReleaseCount.load();
	if(nCurrent >= nCount)
	{
		return;
	}
	uint32_t nNewCount = nCount - nCurrent;
	nReleaseCount.fetch_add(nNewCount);
	if(nNewCount < nNumThreads)
	{
		for(uint32_t i = 0;i < nNewCount; ++i)
		{
			Cond.notify_one();
		}
	}
	else
	{
		Cond.notify_all();
	}

}
void JqSemaphore::Wait()
{
	JQ_MICROPROFILE_SCOPE("void JqSemaphore::Wait()", 0xff);
	{
		//atomic decrement, passthrough if hitting 0
		//return if decrement succeds
		uint32_t nCurrent = nReleaseCount.load();
		while(nCurrent)
		{
			if(nReleaseCount.compare_exchange_weak(nCurrent, nCurrent-1))
			{
				return;
			}
		}
	}
	{
		JQ_MICROPROFILE_SCOPE("SemaWaitLock", 0xff);
		std::unique_lock<std::mutex> lock(Mutex);
		Cond.wait(lock);
	}

	{
		//atomic decrement, passthrough if hitting 0
		//return if decrement succeds
		uint32_t nCurrent = nReleaseCount.load();
		while(nCurrent)
		{
			if(nReleaseCount.compare_exchange_weak(nCurrent, nCurrent-1))
			{
				return;
			}
		}
	}

}
#endif

#endif
