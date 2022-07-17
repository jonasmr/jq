#pragma once

#ifdef JQ_USE_CONFIG_H
#include "jq.config.h"
#endif

#ifndef JQ_JOB_BUFFER_SIZE
#define JQ_JOB_BUFFER_SIZE (2048)
#define JQ_JOB_BUFFER_SHIFT 11 // these are paired and must match
#endif

#ifndef JQ_DEFAULT_WAIT_TIME_US
#define JQ_DEFAULT_WAIT_TIME_US 10
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
#define JQ_MAX_THREADS 128
#endif

#ifndef JQ_ASSERT_SANITY
#define JQ_ASSERT_SANITY 0
#endif

#ifndef JQ_MAX_JOB_STACK
#define JQ_MAX_JOB_STACK 64
#endif

#ifndef JQ_MAX_QUEUES
#define JQ_MAX_QUEUES 8
#endif

#ifndef JQ_API
#define JQ_API
#endif

#ifndef JQ_DEFAULT_STACKSIZE_SMALL
#define JQ_DEFAULT_STACKSIZE_SMALL (16 << 10)
#endif

#ifndef JQ_DEFAULT_STACKSIZE_LARGE
#define JQ_DEFAULT_STACKSIZE_LARGE (128 << 10)
#endif

#ifndef JQ_LOCK_STATS
#define JQ_LOCK_STATS 1
#endif

#ifndef JQ_CHILD_HANDLE_BUFFER_SIZE
#define JQ_CHILD_HANDLE_BUFFER_SIZE 128
#endif

#define JQ_MAX_SUBJOBS (0xfffe) // Max times a single job can run
#define JQ_INVALID_SUBJOB (0xffff)
#define JQ_INVALID_QUEUE 0xff

#ifndef JQ_BREAK
#ifdef _WIN32
#define JQ_BREAK() __debugbreak()
#else
#define JQ_BREAK() __builtin_trap()
#endif
#endif

#ifndef JQ_ASSERT
#ifdef JQ_NO_ASSERT
#define JQ_ASSERTS_ENABLED 0
#define JQ_ASSERT(a)                                                                                                                                                                                   \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#else
#define JQ_ASSERTS_ENABLED 1
#define JQ_ASSERT(a)                                                                                                                                                                                   \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		if(!(a))                                                                                                                                                                                       \
		{                                                                                                                                                                                              \
			JqDump();                                                                                                                                                                                  \
			JQ_BREAK();                                                                                                                                                                                \
		}                                                                                                                                                                                              \
	} while(0)
#endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// template stuff to make the job function accept to 0-2 arguments
template <typename T>
struct JqAdapt : public JqAdapt<decltype(&T::operator())>
{
};
template <typename C>
struct JqAdapt<void (C::*)(int, int) const>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t(a, b);
	}
};
template <typename C>
struct JqAdapt<void (C::*)(int) const>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t(a);
	}
};
template <typename C>
struct JqAdapt<void (C::*)() const>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t();
	}
};

template <>
struct JqAdapt<void (*)(int, int)>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t(a, b);
	}
};
template <>
struct JqAdapt<void (*)(int)>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t(a);
	}
};
template <>
struct JqAdapt<void (*)()>
{
	template <typename T>
	void call(T& t, int a, int b)
	{
		t();
	}
};

// minimal lambda implementation without support for non-trivial types
// and fixed memory footprint
struct JqCallableBase
{
	virtual void operator()(int begin, int end) = 0;
};
template <typename F>
struct JqCallable : JqCallableBase
{
	F functor;
	JqCallable(F functor)
		: functor(functor)
	{
	}
	virtual void operator()(int a, int b)
	{
		JqAdapt<F> X;
		X.call(functor, a, b);
	}
};
class JqFunction
{
	union
	{
		char  buffer[JQ_FUNCTION_SIZE];
		void* vptr; // alignment helper and a way to clear the vptr
	};
	JqCallableBase* Base()
	{
		return (JqCallableBase*)&buffer[0];
	}

  public:
	template <typename F>
	JqFunction(F f)
	{
		static_assert(std::is_trivially_copy_constructible<F>::value, "Only captures of trivial types supported.");
		static_assert(std::is_trivially_destructible<F>::value, "Only captures of trivial types supported.");
		static_assert(sizeof(JqCallable<F>) <= JQ_FUNCTION_SIZE, "Captured lambda is too big. Increase size or capture less");
#ifdef _WIN32
		static_assert(__alignof(F) <= __alignof(void*), "Alignment requirements too high");
#else
		static_assert(alignof(F) <= alignof(void*), "Alignment requirements too high");
#endif
		new(Base()) JqCallable<F>(f);
	}
	JqFunction()
	{
	}
	void Clear()
	{
		vptr = 0;
	}
	void operator()(int a, int b)
	{
		(*Base())(a, b);
	}
};
#define JQ_CLEAR_FUNCTION(f)                                                                                                                                                                           \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		f.Clear();                                                                                                                                                                                     \
	} while(0)

///////////////////////////////////////////////////////////////////////////////////////////
/// Interface

//  what to execute while wailing
#define JQ_WAITFLAG_EXECUTE_CHILDREN 0x1
#define JQ_WAITFLAG_EXECUTE_ANY 0x2
#define JQ_WAITFLAG_EXECUTE_PREFER_CHILDREN 0x3
//  what to do when out of jobs
#define JQ_WAITFLAG_BLOCK 0x4
#define JQ_WAITFLAG_SLEEP 0x8
#define JQ_WAITFLAG_SPIN 0x10
#define JQ_WAITFLAG_IGNORE_CHILDREN 0x20

#define JQ_DEFAULT_WAIT_FLAG (JQ_WAITFLAG_EXECUTE_CHILDREN | JQ_WAITFLAG_SPIN)

// Job flags
#define JQ_JOBFLAG_SMALL_STACK 0x1 // create with small stack
#define JQ_JOBFLAG_DETACHED 0x2	   // dont create as child of current job

// Init flags
#define JQ_INIT_USE_SEPERATE_STACK 0x1

struct JqHandle
{
	uint64_t H = 0;
};

struct JqStats
{
	uint32_t nNumAdded;
	uint32_t nNumFinished;
	uint32_t nNumAddedSub;
	uint32_t nNumFinishedSub;
	uint32_t nNumCancelled;
	uint32_t nNumCancelledSub;
	uint32_t nNumLocks;
	uint32_t nNumSema;
	uint32_t nNumLocklessPops;
	uint32_t nNumWaitCond;
	uint32_t nNumWaitKicks;
	uint32_t nMemoryUsed;
	JqHandle nNextHandle;
	uint32_t nSkips;
	uint32_t nAttempts;
	uint32_t nNextHandleCalled;
	void	 Add(JqStats& Other)
	{
		nNumAdded += Other.nNumAdded;
		nNumFinished += Other.nNumFinished;
		nNumAddedSub += Other.nNumAddedSub;
		nNumFinishedSub += Other.nNumFinishedSub;
		nNumCancelled += Other.nNumCancelled;
		nNumCancelledSub += Other.nNumCancelledSub;
		nNumLocks += Other.nNumLocks;
		nNumSema += Other.nNumSema;
		nNumLocklessPops += Other.nNumLocklessPops;
		nMemoryUsed += Other.nMemoryUsed;
		nNextHandle = Other.nNextHandle.H > nNextHandle.H ? Other.nNextHandle : nNextHandle;
		nSkips += Other.nSkips;
		nAttempts += Other.nAttempts;
		nNextHandleCalled += Other.nNextHandleCalled;
	}
};

// Specify order of which to take jobs
struct JqQueueOrder
{
	uint8_t nNumPipes;
	uint8_t Queues[JQ_MAX_QUEUES];
};

struct JqAttributes
{
	uint32_t Flags;
	uint32_t NumWorkers;
	uint32_t StackSizeSmall;
	uint32_t StackSizeLarge;
	uint32_t NumQueueOrders;

	JqQueueOrder QueueOrder[JQ_MAX_THREADS];		   // defines a set of different ways of pulling out work
	uint8_t		 WorkerOrderIndex[JQ_MAX_THREADS];	   // for each worker thread, pick on of the pipe orders from abovce.
	uint64_t	 WorkerThreadAffinity[JQ_MAX_THREADS]; // for each worker, what should the affinity be set to. 0 leaves it as default
};

JQ_API JqHandle JqSelf();
JQ_API JqHandle JqAdd(const char* Name, JqFunction JobFunc, uint8_t Queue, int NumJobs = 1, int Range = -1, uint32_t JobFlags = 0);

// Add a job which has previously been reserved with a call to JqReserve
// This decrements the block counter by 1 once its added
JQ_API JqHandle JqAddReserved(JqHandle ReservedHandle, JqFunction JobFunc, uint8_t Queue, int NumJobs = 1, int Range = -1, uint32_t JobFlags = 0);

// add successor
JQ_API JqHandle JqAddSuccessor(const char* Name, JqHandle Precondition, JqFunction JobFunc, uint8_t Queue, int NumJobs = 1, int Range = -1, uint32_t JobFlags = 0);

// Reserve a job handle. The job handle is created with a block counter value of 1, letting you add other blocking conditions.
// Once done, it can be released with
//  - JqRelease(): No job  will be executed, but it can be used as a barrier by making other jobs depend on this job
//  - JqAddReserved(): The job will be added to the queue once the block counter reaches zero
JQ_API JqHandle JqReserve(const char* Name);

// Set Precondition to be required to be finished before Handle is started.
// Increments the block counter of Handle, and decrements it once its finished
//  * Note that you cannot Increment the block counter once it has reached zero, as it will then be eligible for exection
JQ_API void JqAddPrecondition(JqHandle Handle, JqHandle Precondition);

// Manually increment the block counter by one. Blocks execution of this Handle, until a matching JqRelease is called
//  * Note that you cannot Increment the block counter once it has reached zero, as it will then be eligible for exection
JQ_API void JqBlock(JqHandle Handle);

// Decrement the block counter manually.
// Use this with matching calls to JqBlock, or for JqReserve calls for barrier type jobs. Note that if you call JqAddReserved, it decrements the block counter
JQ_API void JqRelease(JqHandle Handle);

// Similarly to add, add a job, but JqSpawn differs
// - Immediately wait for it, not return before all subjobs are done
// - Will always execute at least job instance 0 on the calling thread.
JQ_API void JqSpawn(const char* Name, JqFunction JobFunc, uint8_t Queue, int NumJobs = 1, int Range = -1, uint32_t WaitFlag = JQ_DEFAULT_WAIT_FLAG);

JQ_API void		JqWait(JqHandle Handle, uint32_t WaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void		JqWaitAll();
JQ_API void		JqWaitAll(JqHandle* Jobs, uint32_t NumJobs, uint32_t WaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t UsWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API JqHandle JqWaitAny(JqHandle* Jobs, uint32_t NumJobs, uint32_t WaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t UsWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void		JqCancel(JqHandle Handle);
JQ_API bool		JqExecuteChild(JqHandle Handle); // execute 1 child job.
JQ_API JqHandle JqGroupBegin(const char* Name);	 // add a non-executing job to group all jobs added between begin/end
JQ_API void		JqGroupEnd();
JQ_API bool		JqIsDone(JqHandle Handle);
JQ_API bool		JqIsStarted(JqHandle Handle);
JQ_API bool		JqIsDoneExt(JqHandle Handle, uint32_t WaitFlag);
JQ_API void		JqStart(int NumWorkers);
JQ_API void		JqStart(JqAttributes* Attributes);
JQ_API void		JqSetThreadQueueOrder(JqQueueOrder* Config);
JQ_API int		JqNumWorkers();
JQ_API void		JqStop();
JQ_API uint32_t JqSelfJobIndex();
JQ_API int		JqGetNumWorkers();
JQ_API void		JqConsumeStats(JqStats* StatsOut);
JQ_API bool		JqExecuteOne();
JQ_API bool		JqExecuteOne(uint8_t Queues);
JQ_API bool		JqExecuteOne(uint8_t* Queues, uint8_t NumQueues);
JQ_API void		JqStartSentinel(int nTimeout);
JQ_API void		JqCrashAndDump();
JQ_API void		JqDump();
JQ_API void		JqInitAttributes(JqAttributes* Attributes, uint32_t NumQueueOrders = 0, uint32_t NumWorkers = 0);
JQ_API int64_t	JqGetTicksPerSecond();
JQ_API int64_t	JqGetTick();

JQ_API void JqOnThreadExit(); // this must be called from each thread calling Jq functions. called automatically from the worker threads

// Helper / debug functions
JQ_API uint64_t JqGetCurrentThreadId(); // for debugging.
JQ_API void		JqUSleep(uint64_t us);
JQ_API void		JqLogStats();
JQ_API uint64_t JqGetCurrentThreadId();
JQ_API uint32_t JqGetNumCpus();

#define JQ_GRAPH_FLAG_SHOW_WAITS 0x1
#define JQ_GRAPH_FLAG_SHOW_PRECONDITIONS 0x2
// Start/Stop generating a graphwiz file from the jobs & dependencies added
JQ_API void JqGraphDumpStart(const char* Filename, uint32_t GraphBufferSize, uint32_t GraphFlags = (JQ_GRAPH_FLAG_SHOW_PRECONDITIONS | JQ_GRAPH_FLAG_SHOW_WAITS));
JQ_API void JqGraphDumpEnd();
