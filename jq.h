#pragma once

#ifndef JQ_WORK_BUFFER_SIZE
#define JQ_WORK_BUFFER_SIZE (2048)
#endif

#ifndef JQ_PRIORITY_MAX
#define JQ_PRIORITY_MAX 8
#endif

#ifndef JQ_MAX_JOB_STACK
#define JQ_MAX_JOB_STACK 8
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
#define JQ_MAX_THREADS 64
#endif

#ifndef JQ_ASSERT_SANITY
#define JQ_ASSERT_SANITY 0
#endif

#ifndef JQ_NUM_PIPES
#define JQ_NUM_PIPES 8
#endif

#ifndef JQ_API
#define JQ_API
#endif

#ifndef JQ_STACKSIZE_SMALL
#define JQ_STACKSIZE_SMALL (16<<10)
#endif

#ifndef JQ_STACKSIZE_LARGE
#define JQ_STACKSIZE_LARGE (128<<10)
#endif

#ifndef JQ_USE_SEPERATE_STACK
#define JQ_USE_SEPERATE_STACK 1
#endif

#ifndef JQ_PIPE_BUFFER_SIZE
#define JQ_PIPE_BUFFER_SIZE (2048)
#endif

#ifndef JQ_CACHE_LINE_SIZE
#define JQ_CACHE_LINE_SIZE 64
#endif

#ifdef _WIN32
#define JQ_BREAK() __debugbreak()
#else
#define JQ_BREAK() __builtin_trap()
#endif

#ifdef JQ_NO_ASSERT
#define JQ_ASSERT(a) do{}while(0)
#else
#define JQ_ASSERT(a) do{if(!(a)){JqDump(); JQ_BREAK();} }while(0)
#endif




#include <type_traits>
#include <stddef.h>
#include <stdint.h>

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
		static_assert(std::is_trivially_copy_constructible<F>::value, "Only captures of trivial types supported.");
		static_assert(std::is_trivially_destructible<F>::value, "Only captures of trivial types supported.");
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



///////////////////////////////////////////////////////////////////////////////////////////
/// Interface

//  what to execute while wailing
#define JQ_WAITFLAG_EXECUTE_SUCCESSORS 			0x1
#define JQ_WAITFLAG_EXECUTE_ANY 				0x2
#define JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS 	0x3
//  what to do when out of jobs
#define JQ_WAITFLAG_BLOCK 						0x4
#define JQ_WAITFLAG_SLEEP 						0x8
#define JQ_WAITFLAG_SPIN 						0x10
#define JQ_WAITFLAG_IGNORE_CHILDREN 			0x20

#define JQ_DEFAULT_WAIT_FLAG (JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SPIN)

//Job flags 
#define JQ_JOBFLAG_LARGE_STACK 					0x1 // create with large stack
#define JQ_JOBFLAG_DETACHED 					0x2 // dont create as child of current job 


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
	uint32_t nMemoryUsed;
	uint64_t nNextHandle;
	uint32_t nSkips;
	uint32_t nAttempts;
	uint32_t nNextHandleCalled;
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
		nMemoryUsed += Other.nMemoryUsed;
		nNextHandle += Other.nNextHandle;
		nSkips += Other.nSkips;
		nAttempts += Other.nAttempts;
		nNextHandleCalled += Other.nNextHandleCalled;
	}
};


JQ_API uint64_t		JqSelf();
JQ_API uint64_t 	JqAdd(JqFunction JobFunc, uint8_t nPipe, int nNumJobs = 1, int nRange = -1, uint32_t nJobFlags = 0);
JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPipe, int nNumJobs = 1, int nRange = -1, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG);
JQ_API void 		JqWait(uint64_t nJob, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API void			JqWaitAll();
JQ_API void 		JqWaitAll(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API uint64_t		JqWaitAny(uint64_t* pJobs, uint32_t nNumJobs, uint32_t nWaitFlag = JQ_DEFAULT_WAIT_FLAG, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
JQ_API bool 		JqCancel(uint64_t nJob);
JQ_API void			JqExecuteChildren(uint64_t nJob);
JQ_API uint64_t 	JqGroupBegin(); //add a non-executing job to group all jobs added between begin/end
JQ_API void 		JqGroupEnd();
JQ_API bool 		JqIsDone(uint64_t nJob);
JQ_API bool 		JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlag);
JQ_API void 		JqStart(int nNumWorkers);
JQ_API void 		JqStart(int nNumWorkers, uint32_t nPipeConfigSize, uint8_t* pPipeConfig);
JQ_API void			JqSetThreadPipeConfig(uint8_t PipeConfig[JQ_NUM_PIPES]);
JQ_API int			JqNumWorkers();
JQ_API void 		JqStop();
JQ_API uint32_t		JqSelfJobIndex();
JQ_API int 			JqGetNumWorkers();
JQ_API void 		JqConsumeStats(JqStats* pStatsOut);
JQ_API bool			JqExecuteOne(int nShortOnly);
JQ_API void 		JqStartSentinel(int nTimeout);
JQ_API void 		JqCrashAndDump();
JQ_API void 		JqDump();
