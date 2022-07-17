#pragma once
// internal code, shared by both lockless and locked version
#include "jq.h"
#include "jqfcontext.h"
#ifdef JQ_MICROPROFILE
#include "microprofile.h"
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
//#include <libkern/OSAtomic.h>
#include <os/lock.h>
#include <pthread.h>
#include <unistd.h>
#define JQ_BREAK() __builtin_trap()
#define JQ_THREAD_LOCAL __thread
#define JQ_STRCASECMP strcasecmp
typedef uint64_t ThreadIdType;
#define JQ_USLEEP(us) usleep(us);
inline int64_t JqTicksPerSecond()
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
inline int64_t JqTick()
{
	return mach_absolute_time();
}

inline uint64_t JqCurrentThreadId()
{
	uint64_t tid;
	pthread_threadid_np(pthread_self(), &tid);
	return tid;
}
inline void JqUSleepImpl(uint64_t usec)
{
	usleep(usec);
}

#elif defined(_WIN32)
#define JQ_BREAK() __debugbreak()
#define JQ_THREAD_LOCAL __declspec(thread)
#define JQ_STRCASECMP _stricmp
typedef uint32_t ThreadIdType;
#define JqCurrentThreadId() GetCurrentThreadId()
#include <windows.h>
inline int64_t	 JqTicksPerSecond()
{
	static int64_t nTicksPerSecond = 0;
	if(nTicksPerSecond == 0)
	{
		QueryPerformanceFrequency((LARGE_INTEGER*)&nTicksPerSecond);
	}
	return nTicksPerSecond;
}
inline int64_t JqTick()
{
	int64_t ticks;
	QueryPerformanceCounter((LARGE_INTEGER*)&ticks);
	return ticks;
}
inline void JqUSleepImpl(uint64_t usec)
{
	if(usec > 20000)
	{
		Sleep((DWORD)(usec / 1000));
	}
	else if(usec >= 1000)
	{
#ifdef _DURANGO
		Sleep((DWORD)(usec / 1000));
#else
		timeBeginPeriod(1);
		Sleep((DWORD)(usec / 1000));
		timeEndPeriod(1);
#endif
	}
	else
	{
		Sleep(0);
	}
}

#else

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define JQ_BREAK() __builtin_trap()
#define JQ_THREAD_LOCAL __thread
#define JQ_STRCASECMP strcasecmp
typedef uint64_t ThreadIdType;
#define JQ_USLEEP(us) usleep(us);
#define JqCurrentThreadId() (uint64_t) pthread_self()
inline int64_t	 JqTicksPerSecond()
{
	return 1000000000ll;
}
inline int64_t JqTick()
{
	timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return 1000000000ll * ts.tv_sec + ts.tv_nsec;
}

inline void JqUSleepImpl(uint64_t usec)
{
	usleep(usec);
}

#endif

#ifndef JQ_THREAD
#include <thread>
#define JQ_THREAD std::thread
#define JQ_THREAD_CREATE(pThread)                                                                                                                                                                      \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#define JQ_THREAD_DESTROY(pThread)                                                                                                                                                                     \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#define JQ_THREAD_START(pThread, entry, index)                                                                                                                                                         \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		*pThread = std::thread(entry, index);                                                                                                                                                          \
	} while(0)
#define JQ_THREAD_JOIN(pThread)                                                                                                                                                                        \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		(pThread)->join();                                                                                                                                                                             \
	} while(0)
#endif

// #ifdef JQ_NO_ASSERT
// #define JQ_ASSERT(a) do{}while(0)
// #else
// #define JQ_ASSERT(a) do{if(!(a)){JqDump(); JQ_BREAK();} }while(0)
// #endif

#if JQ_ASSERTS_ENABLED
#define JQ_AL(exp)                                                                                                                                                                                     \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		exp;                                                                                                                                                                                           \
	} while(0)
#define JQ_ASSERT_LOCKED(m)                                                                                                                                                                            \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		if(!m.IsLocked())                                                                                                                                                                              \
		{                                                                                                                                                                                              \
			JQ_BREAK();                                                                                                                                                                                \
		}                                                                                                                                                                                              \
	} while(0)
#define JQ_ASSERT_NOT_LOCKED(m)                                                                                                                                                                        \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		if(m.IsLocked())                                                                                                                                                                               \
		{                                                                                                                                                                                              \
			JQ_BREAK();                                                                                                                                                                                \
		}                                                                                                                                                                                              \
	} while(0)
#else
#define JQ_AL(exp)                                                                                                                                                                                     \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#define JQ_ASSERT_LOCKED(m)                                                                                                                                                                            \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#define JQ_ASSERT_NOT_LOCKED(m)                                                                                                                                                                        \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#endif

#ifdef JQ_MICROPROFILE
#define JQ_MICROPROFILE_SCOPE(a, c) MICROPROFILE_SCOPEI("JQ", a, c)
#else
#define JQ_MICROPROFILE_SCOPE(a, c)                                                                                                                                                                    \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#endif

#ifdef JQ_MICROPROFILE_VERBOSE
#define JQ_MICROPROFILE_VERBOSE_SCOPE(a, c) MICROPROFILE_SCOPEI("JQ", a, c)
#else
#define JQ_MICROPROFILE_VERBOSE_SCOPE(a, c)                                                                                                                                                            \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#endif

#define JQ_LT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b)) < 0)
#define JQ_LE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b)) <= 0)
#define JQ_GE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b)) >= 0)
#define JQ_GT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b)) > 0)
#define JQ_LT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a << (bits)) - (uint64_t)(b << (bits)))) < 0)
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a << (bits)) - (uint64_t)(b << (bits)))) <= 0)
#define JQ_GE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a << (bits)) - (uint64_t)(b << (bits)))) >= 0)
#define JQ_GT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a << (bits)) - (uint64_t)(b << (bits)))) > 0)

#define JQ_GET_INDEX(H) ((H) % (JQ_JOB_BUFFER_SIZE))

#ifdef _WIN32
#define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_16 __declspec(align(16))
#else
#define JQ_ALIGN_CACHELINE __attribute__((__aligned__(JQ_CACHE_LINE_SIZE)))
#define JQ_ALIGN_16 __attribute__((__aligned__(16)))
#endif

#ifndef _WIN32
#include <pthread.h>
#endif
#include <atomic>

#if JQ_LOCK_STATS
#define JQLSC(exp) exp
extern std::atomic<uint32_t> g_JqLockOps;
extern std::atomic<uint32_t> g_JqCondWait;
extern std::atomic<uint32_t> g_JqCondSignal;
extern std::atomic<uint32_t> g_JqSemaSignal;
extern std::atomic<uint32_t> g_JqSemaWait;
extern std::atomic<uint32_t> g_JqLocklessPops;
#else
#define JQLSC(exp)                                                                                                                                                                                     \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#endif

struct JqPipe;
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

#if JQ_ASSERTS_ENABLED
	uint32_t	 nLockCount;
	ThreadIdType nThreadId;
	bool		 IsLocked();
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
	pthread_cond_t	Cond;
#endif
};

#if defined(__APPLE__)
#define JQ_SEMAPHORE_DEFAULT
#elif defined(_WIN32)
#define JQ_SEMAPHORE_WIN32
#else
#define JQ_SEMAPHORE_FUTEX
#endif

struct JqSemaphore
{
	JqSemaphore();
	~JqSemaphore();
	void Signal(uint32_t nCount);
	void Wait();

	void Init(int nMaxCount);

#if defined(JQ_SEMAPHORE_DEFAULT)
	JqMutex				  Mutex;
	JqConditionVariable	  Cond;
	std::atomic<uint32_t> nReleaseCount;
	uint32_t			  nMaxCount;
#elif defined(JQ_SEMAPHORE_WIN32)
	HANDLE			Handle;
	LONG			nMaxCount;
#elif defined(JQ_SEMAPHORE_FUTEX)
	std::atomic<uint32_t> Futex;
	uint32_t			  MaxCount;
#endif
};

struct JqMutexLock
{
	bool	 bIsLocked;
	JqMutex& Mutex;
	JqMutexLock(JqMutex& Mutex)
		: Mutex(Mutex)
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
		bIsLocked = true;
	}
	void Unlock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
		Mutex.Unlock();
		bIsLocked = false;
	}
};

// Scope Mutex helper, which will also assert if the same thread is trying to lock two mutexes
struct JqSingleMutexLock
{
	bool	 bIsLocked;
	JqMutex& Mutex;
	JqSingleMutexLock(JqMutex& Mutex)
		: Mutex(Mutex)
	{
		bIsLocked = false;
		Lock();
	}
	~JqSingleMutexLock()
	{
		if(bIsLocked)
		{
			Unlock();
		}
	}
	void Lock();
	void Unlock();
};

class JqFunction;

struct JqJobStack
{
	uint64_t   GUARD[2];
	JqFContext ContextReturn;
	JqFContext ContextJob;

	JqJobStack* Link; // when in use: Previous. When freed, next element in free list
	JqFunction* Function;

	uint32_t Flags;

	int	  Begin;
	int	  End;
	int	  Size;
	void* StackBottom()
	{
		intptr_t t = (intptr_t)this;
		t += Offset();
		t -= Size;
		return (void*)t;
	}
	void* StackTop()
	{
		intptr_t t = (intptr_t)this;
		t -= 16;
		return (void*)this;
	}
	int StackSize()
	{
		return (int)((intptr_t)StackTop() - (intptr_t)StackBottom());
	}
	static size_t Offset()
	{
		return (sizeof(JqJobStack) + 15) & (~15);
	}
	static JqJobStack* Init(void* Stack, int StackSize, uint32_t Flags)
	{
		intptr_t t = (intptr_t)Stack;
		t += StackSize;
		t -= Offset();
		JqJobStack* JobStack = (JqJobStack*)t;
		new(JobStack) JqJobStack;
		JobStack->Link	   = 0;
		JobStack->Function = 0;
		JobStack->Flags	   = Flags;
		JobStack->Size	   = StackSize;
		JobStack->GUARD[0] = (uint64_t)0xececececececececll;
		JobStack->GUARD[1] = (uint64_t)0xececececececececll;
		return JobStack;
	}
};

struct JqJobStackLink
{
	JqJobStack* pHead;
	uint32_t	nCounter;
};

typedef std::atomic<JqJobStackLink> JqJobStackList;

void* JqAllocStackInternal(uint32_t nStackSize);
void  JqFreeStackInternal(void* pStack, uint32_t nStackSize);

JqJobStack* JqAllocStack(JqJobStackList& FreeList, uint32_t nStackSize, uint32_t nFlags);
void		JqFreeStack(JqJobStackList& FreeList, JqJobStack* pStack);
void		JqFreeAllStacks(JqJobStackList& FreeList);
uint32_t	JqGetNumCpus();
void		JqSetThreadAffinity(uint64_t Affinity);

JqMutex** JqGetSingleMutexPtr();
