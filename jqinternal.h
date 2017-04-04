#pragma once
//internal code, shared by both lockless and locked version

#include "jqfcontext.h"
#ifdef JQ_MICROPROFILE
#include "microprofile.h"
#endif


#if defined(__APPLE__)
#include <mach/mach_time.h>
//#include <libkern/OSAtomic.h>
#include <os/lock.h>
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
#elif defined(_WIN32)
#define JQ_BREAK() __debugbreak()
#define JQ_THREAD_LOCAL __declspec(thread)
#define JQ_STRCASECMP _stricmp
typedef uint32_t ThreadIdType;
#define JQ_USLEEP(us) JqUsleep(us);
#include <windows.h>
inline int64_t JqTicksPerSecond()
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
inline void JqUsleep(__int64 usec) 
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
#define JQ_ASSERT(a) do{if(!(a)){JqDump(); JQ_BREAK();} }while(0)
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

#define JQ_LT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<0)
#define JQ_LE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<=0)
#define JQ_GE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))>=0)
#define JQ_GT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))>0)
#define JQ_LT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<0)
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<=0)
#define JQ_GE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))>=0)
#define JQ_GT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))>0)

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
#define JQLSC(exp) do{}while(0)
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
#if defined(__APPLE__)
	os_unfair_lock UnfairLock;
	std::atomic<uint64_t> Owner;
	std::atomic<uint32_t> Count;
#else	
	pthread_mutex_t Mutex;
#endif
#endif
};

#if defined(__APPLE__)
struct JqCondMutex
{
	JqCondMutex();
	~JqCondMutex();
	void Lock();
	void Unlock();
	pthread_mutex_t Mutex;
};
#else
typedef JqMutex JqCondMutex;
#endif

struct JqConditionVariable
{
	JqConditionVariable();
	~JqConditionVariable();
	void Wait(JqCondMutex& Mutex);
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
	JqCondMutex Mutex;
	JqConditionVariable Cond;
	std::atomic<uint32_t> nReleaseCount;
	uint32_t nMaxCount;	
#endif
};

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



struct JqCondMutexLock
{
	bool bIsLocked;
	JqCondMutex& Mutex;
	JqCondMutexLock(JqCondMutex& Mutex)
		:Mutex(Mutex)
	{
		bIsLocked = false;
		Lock();
	}
	~JqCondMutexLock()
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



struct JqJobStack
{
	uint64_t GUARD[2];
	JqFContext ContextReturn;
	JqFContext pContextJob;

	JqJobStack* pLink;//when in use: Previous. When freed, next element in free list
	JqPipe* pPipe;

	uint32_t nExternalId;
	uint32_t nFlags;
	int nBegin;
	int nEnd;
	int nStackSize;
	void* StackBottom()
	{
		intptr_t t = (intptr_t)this;
		t += Offset();
		t -= nStackSize;
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
	static JqJobStack* Init(void* pStack, int nStackSize, uint32_t nFlags)
	{
		intptr_t t = (intptr_t)pStack;
		t += nStackSize;
		t -= Offset();
		JqJobStack* pJobStack = (JqJobStack*)t;
		new (pJobStack) JqJobStack;
		pJobStack->pLink = 0;
		pJobStack->nExternalId = 0;
		pJobStack->nFlags = nFlags;
		pJobStack->nStackSize = nStackSize;
		pJobStack->GUARD[0] = (uint64_t)0xececececececececll;
		pJobStack->GUARD[1] = (uint64_t)0xececececececececll;
		return pJobStack;
	}
};


struct JqJobStackLink
{
	JqJobStack* pHead;
	uint32_t 	nCounter;
};

typedef std::atomic<JqJobStackLink> JqJobStackList;


void* JqAllocStackInternal(uint32_t nStackSize);
void JqFreeStackInternal(void* pStack, uint32_t nStackSize);

JqJobStack* JqAllocStack(JqJobStackList& FreeList, uint32_t nStackSize, uint32_t nFlags);
void JqFreeStack(JqJobStackList& FreeList, JqJobStack* pStack);
void JqFreeAllStacks(JqJobStackList& FreeList);




