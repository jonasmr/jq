#include "jqinternal.h"
#include "jq.h"
#include <stdint.h>

static JQ_THREAD_LOCAL JqMutex* g_SingleMutexLockMutex = nullptr;

struct JqLocalJobStack
{
	JqJobStackList* FreeList;
	JqJobStack*		Stack;
};

struct JqLocalJobStacks
{
	JqLocalJobStack Stack[2];
};

static JQ_THREAD_LOCAL JqLocalJobStacks g_ThreadLocalStacks;

#if JQ_LOCK_STATS
std::atomic<uint32_t> g_JqLockOps;
std::atomic<uint32_t> g_JqCondWait;
std::atomic<uint32_t> g_JqCondSignal;
std::atomic<uint32_t> g_JqSemaSignal;
std::atomic<uint32_t> g_JqSemaWait;
std::atomic<uint32_t> g_JqLocklessPops;
#endif

#ifdef _WIN32
#ifndef CreateSemaphoreEx
#define CreateSemaphoreEx CreateSemaphoreExW
#endif
JqMutex::JqMutex()
{

	JQ_AL(nLockCount = 0);
	JQ_AL(nThreadId = 0);
	InitializeCriticalSection(&CriticalSection);
}

JqMutex::~JqMutex()
{
	DeleteCriticalSection(&CriticalSection);
}

void JqMutex::Lock()
{
	EnterCriticalSection(&CriticalSection);
	// printf("JqMutex::LOCK   %p\n", this);
	JQLSC(g_JqLockOps.fetch_add(1));
	JQ_AL(nThreadId = JqCurrentThreadId());
	JQ_AL(nLockCount++);
}

void JqMutex::Unlock()
{
#ifdef JQ_ASSERT_LOCKS
	nLockCount--;
	if(0 == nLockCount)
	{
		nThreadId = 0;
	}
#endif
	// printf("JqMutex::UNLOCK %p\n", this);
	LeaveCriticalSection(&CriticalSection);
	JQLSC(g_JqLockOps.fetch_add(1));
}

#ifdef JQ_ASSERT_LOCKS
bool JqMutex::IsLocked()
{
	return (nThreadId == JqCurrentThreadId());
}
#endif
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
	JQLSC(g_JqCondWait.fetch_add(1));

#ifdef JQ_ASSERT_LOCKS
	Mutex.nLockCount--;
	if(0 == Mutex.nLockCount)
	{
		Mutex.nThreadId = 0;
	}
#endif
	SleepConditionVariableCS(&Cond, &Mutex.CriticalSection, INFINITE);

	JQ_AL(Mutex.nThreadId = JqCurrentThreadId());
	JQ_AL(Mutex.nLockCount++);
}

void JqConditionVariable::NotifyOne()
{
	JQLSC(g_JqCondSignal.fetch_add(1));
	WakeConditionVariable(&Cond);
}

void JqConditionVariable::NotifyAll()
{
	JQLSC(g_JqCondSignal.fetch_add(1));
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
	Handle	  = CreateSemaphoreEx(NULL, 0, nCount * 2, NULL, 0, SEMAPHORE_ALL_ACCESS);
}

void JqSemaphore::Signal(uint32_t nCount)
{
	if(nCount > (uint32_t)nMaxCount)
		nCount = nMaxCount;
	BOOL r = ReleaseSemaphore(Handle, nCount, 0);
	(void)r;
	JQLSC(g_JqSemaSignal.fetch_add(1));
}

void JqSemaphore::Wait()
{
	JQLSC(g_JqSemaWait.fetch_add(1));
	JQ_MICROPROFILE_SCOPE("Wait", 0xc0c0c0);
	DWORD r = WaitForSingleObject((HANDLE)Handle, INFINITE);
	JQ_ASSERT(WAIT_OBJECT_0 == r);
}
#else

JqMutex::JqMutex()
{
#ifdef JQ_ASSERT_LOCKS
	nLockCount = 0;
	nThreadId  = 0;
#endif

	pthread_mutex_init(&Mutex, 0);
}

JqMutex::~JqMutex()
{
	pthread_mutex_destroy(&Mutex);
}

void JqMutex::Lock()
{
	JQ_ASSERT(nThreadId != JqCurrentThreadId());
	pthread_mutex_lock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));
	// printf("JqMutex::LOCK   %p  tid: %llx\n", this, JqCurrentThreadId());

	JQ_AL(nThreadId = JqCurrentThreadId());
	JQ_AL(nLockCount++);
}

void JqMutex::Unlock()
{
#ifdef JQ_ASSERT_LOCKS
	nLockCount--;
	if(0 == nLockCount)
	{
		nThreadId = 0;
	}
#endif
	// printf("JqMutex::UNLOCK %p  tid: %llx\n", this, JqCurrentThreadId());

	pthread_mutex_unlock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));
}

#ifdef JQ_ASSERT_LOCKS
bool JqMutex::IsLocked()
{
	return nThreadId == JqCurrentThreadId();
}
#endif

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

#ifdef JQ_ASSERT_LOCKS
	Mutex.nLockCount--;
	if(0 == Mutex.nLockCount)
	{
		Mutex.nThreadId = 0;
	}
#endif

	pthread_cond_wait(&Cond, &Mutex.Mutex);

	JQ_AL(Mutex.nThreadId = JqCurrentThreadId());
	JQ_AL(Mutex.nLockCount++);

	JQLSC(g_JqCondWait.fetch_add(1));
}

void JqConditionVariable::NotifyOne()
{
	pthread_cond_signal(&Cond);
	JQLSC(g_JqCondSignal.fetch_add(1));
}

void JqConditionVariable::NotifyAll()
{
	pthread_cond_broadcast(&Cond);
	JQLSC(g_JqCondSignal.fetch_add(1));
}

JqSemaphore::JqSemaphore()
{
	nMaxCount = 0xffffffff;
	nReleaseCount.store(0);
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
	JQLSC(g_JqSemaSignal.fetch_add(1));

	if(nReleaseCount.load() == nMaxCount)
	{
		return;
	}
	{
		JqMutexLock l(Mutex);
		uint32_t	nCurrent = nReleaseCount.load();
		if(nCurrent + nCount > nMaxCount)
			nCount = nMaxCount - nCurrent;
		nReleaseCount.fetch_add(nCount);
		JQ_ASSERT(nReleaseCount.load() <= nMaxCount);
		if(nReleaseCount.load() == nMaxCount)
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
	JQLSC(g_JqSemaWait.fetch_add(1));

	JQ_MICROPROFILE_SCOPE("Wait", 0xc0c0c0);
	JqMutexLock l(Mutex);
	while(!nReleaseCount)
	{
		Cond.Wait(Mutex);
	}
	nReleaseCount--;
}

#endif

#ifndef JQ_ALLOC_STACK_INTERNAL_IMPL
#ifdef _WIN32
void* JqAllocStackInternal(uint32_t nStackSize)
{
	void* pAddr = VirtualAlloc(0, nStackSize, MEM_RESERVE, PAGE_READWRITE);
	VirtualAlloc(pAddr, nStackSize, MEM_COMMIT, PAGE_READWRITE);
	return pAddr;
}
void JqFreeStackInternal(void* pStack, uint32_t nStackSize)
{
	(void)nStackSize;
	VirtualFree(pStack, 0, MEM_RELEASE);
}
#else
#include <stdlib.h>
#include <sys/mman.h>
void* JqAllocStackInternal(uint32_t nStackSize)
{
	int	  nPageSize = sysconf(_SC_PAGE_SIZE);
	int	  nSize		= (nStackSize + nPageSize - 1) & ~(nPageSize - 1);
	void* pAlloc	= mmap(nullptr, nSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
	return pAlloc;
}

void JqFreeStackInternal(void* p, uint32_t nStackSize)
{
	munmap(p, nStackSize);
}
#endif
#endif

JqJobStack* JqAllocStack2(JqJobStackList& FreeList, uint32_t nStackSize, uint32_t nFlags)
{

	// JqJobStack*&
	// static JQ_THREAD_LOCAL JqJobStack* g_ThreadLocalSmallStack = nullptr;
	// static JQ_THREAD_LOCAL JqJobStack* g_ThreadLocalLargeStack = nullptr;

	// struct JqLocalJobStack
	// {
	// 	uint32_t StackSize;
	// 	JqJobStack* Stacks;
	// };

	// struct JqLocalJobStacks
	// {
	// 	JqLocalJobStack Stacks[2];
	// };

	// static JQ_THREAD_LOCAL JqLocalJobStacs g_ThreadLocalStacks = nullptr;

	do
	{
		JqJobStackLink Value = FreeList.load();
		JqJobStack*	   pHead = Value.pHead;

		if(!pHead)
			break;

		JqJobStack*	   pNext	= pHead->pLink;
		JqJobStackLink NewValue = { pNext, Value.nCounter + 1 };
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
			JQ_ASSERT((nFlags & JQ_JOBFLAG_SMALL_STACK) == (pHead->nFlags & JQ_JOBFLAG_SMALL_STACK));
			pHead->pLink = nullptr;
			return pHead;
		}
	} while(1);

#ifdef JQ_MICROPROFILE
	MICROPROFILE_COUNTER_ADD("jq/stack/count", 1);
	MICROPROFILE_COUNTER_ADD("jq/stack/bytes", nStackSize);
#endif
	void*		pStack	  = JqAllocStackInternal(nStackSize);
	JqJobStack* pJobStack = JqJobStack::Init(pStack, nStackSize, nFlags & JQ_JOBFLAG_SMALL_STACK);
	return pJobStack;
}
void JqFreeAllStacks(JqJobStackList& FreeList)
{
	do
	{
		JqJobStackLink Value = FreeList.load();
		JqJobStack*	   pHead = Value.pHead;
		if(!pHead)
		{
			return;
		}
		JqJobStackLink NewValue = { pHead->pLink, Value.nCounter + 1 };
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
#ifdef JQ_MICROPROFILE
			MICROPROFILE_COUNTER_SUB("jq/stack/count", 1);
			MICROPROFILE_COUNTER_SUB("jq/stack/bytes", pHead->nStackSize);
#endif
			JqFreeStackInternal(pHead->StackBottom(), pHead->nStackSize);
		}

	} while(1);
}

void JqFreeStack2(JqJobStackList& FreeList, JqJobStack* pStack)
{
	MICROPROFILE_SCOPEI("JQ", "JqFreeStack", MP_AUTO);
	JQ_ASSERT(pStack->pLink == nullptr);
	do
	{
		JqJobStackLink Value	= FreeList.load();
		JqJobStack*	   pHead	= Value.pHead;
		pStack->pLink			= pHead;
		JqJobStackLink NewValue = { pStack, Value.nCounter + 1 };
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
			return;
		}

	} while(1);
}

JqJobStack* JqAllocStack(JqJobStackList& FreeList, uint32_t nStackSize, uint32_t nFlags)
{
	MICROPROFILE_SCOPEI("JQ", "JqAllocStack", MP_AUTO);

	JqLocalJobStack& LocalStack = g_ThreadLocalStacks.Stack[0].FreeList == &FreeList ? g_ThreadLocalStacks.Stack[0] : g_ThreadLocalStacks.Stack[1];
	if(LocalStack.FreeList == &FreeList)
	{
		JQ_ASSERT(LocalStack.Stack != nullptr);
		// reuse stack
		JqJobStack* JobStack = LocalStack.Stack;
		LocalStack.FreeList	 = 0;
		LocalStack.Stack	 = nullptr;

		return JobStack;
	}
	return JqAllocStack2(FreeList, nStackSize, nFlags);
}

void JqFreeStack(JqJobStackList& FreeList, JqJobStack* pStack)
{
	MICROPROFILE_SCOPEI("JQ", "JqFreeStack", MP_AUTO);
	JqLocalJobStack& LocalStack = g_ThreadLocalStacks.Stack[0].FreeList == &FreeList ? g_ThreadLocalStacks.Stack[0] : g_ThreadLocalStacks.Stack[1];
	if(LocalStack.FreeList == &FreeList)
	{
		JqFreeStack2(FreeList, pStack);
	}
	else
	{
		if(LocalStack.FreeList)
		{
			JqFreeStack2(*LocalStack.FreeList, LocalStack.Stack);
		}
		LocalStack.FreeList = &FreeList;
		LocalStack.Stack	= pStack;
	}
}

void JqInitAttributes(JqAttributes* Attributes, uint32_t NumQueueOrders, uint32_t NumWorkers)
{
	JQ_ASSERT(NumWorkers <= JQ_MAX_THREADS);
	memset(Attributes, 0, sizeof(*Attributes));
	memset(&Attributes->QueueOrder, 0xff, sizeof(Attributes->QueueOrder));
	memset(&Attributes->WorkerOrderIndex, 0xff, sizeof(Attributes->WorkerOrderIndex));

	Attributes->NumWorkers	   = NumWorkers;
	Attributes->NumQueueOrders = NumQueueOrders;
	Attributes->StackSizeSmall = JQ_DEFAULT_STACKSIZE_SMALL;
	Attributes->StackSizeLarge = JQ_DEFAULT_STACKSIZE_LARGE;

	for(uint32_t i = 0; i < NumQueueOrders; ++i)
	{
		JqQueueOrder& C = Attributes->QueueOrder[i];
		memset(&C.Queues[0], 0xff, sizeof(C.Queues));
		for(uint32_t j = 0; j < JQ_NUM_QUEUES; ++j)
		{
			C.Queues[j] = (uint8_t)j;
		}
		C.nNumPipes = JQ_NUM_QUEUES;
	}
	for(uint32_t i = 0; i < NumWorkers; ++i)
	{
		Attributes->WorkerOrderIndex[i] = 0;
	}
}

void JqStart(int NumWorkers)
{
	JqAttributes Attr;
	JqInitAttributes(&Attr, 1, NumWorkers);
	JqStart(&Attr);
}

int64_t JqGetTicksPerSecond()
{
	return JqTicksPerSecond();
}

int64_t JqGetTick()
{
	return JqTick();
}

uint64_t JqGetCurrentThreadId()
{
	return JqCurrentThreadId();
}

void JqUSleep(uint64_t usec)
{
	JqUSleepImpl(usec);
}

void JqSingleMutexLock::Lock()
{
	JQ_ASSERT(g_SingleMutexLockMutex == nullptr);
	JQ_MICROPROFILE_VERBOSE_SCOPE("MutexLock", 0x992233);
	Mutex.Lock();
	bIsLocked			   = true;
	g_SingleMutexLockMutex = &Mutex;
}
void JqSingleMutexLock::Unlock()
{
	g_SingleMutexLockMutex = nullptr;
	JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
	Mutex.Unlock();
	bIsLocked = false;
}
static bool		g_LogResetStats = false;
static uint64_t g_LogTickLast	= 0;
static JqStats	g_LogStats;
void			JqLogResetStats()
{
	g_LogResetStats = true;
}

// helper to log stats every
void JqLogStats()
{
	static int	 Frames = 0;
	static float fLimit = 5;
	static bool	 bFirst = true;

	// JqStats& Stats = g_LogStats;

	if(bFirst || g_LogResetStats)
	{
		g_LogResetStats = false;
		bFirst			= false;
		memset(&g_LogStats, 0, sizeof(g_LogStats));
		g_LogTickLast = JqTick();
		printf("\n");
	}

	if(Frames++ > fLimit)
	{
		JqStats Stats;
		JqConsumeStats(&Stats);
		g_LogStats.Add(Stats);
		static bool		bFirst			   = true;
		static JqHandle H				   = g_LogStats.nNextHandle;
		uint64_t		nHandleConsumption = g_LogStats.nNextHandle.H - H.H;
		H								   = g_LogStats.nNextHandle;

		bool bUseWrapping = true;
		if(bFirst)
		{
			bFirst		 = false;
			bUseWrapping = false;
			printf("\n|Per ms  %10s/%10s/%10s, %10s/%10s|%8s %8s %8s|Total %8s/%8s, %14s/%14s|%8s|%13s|%7s|%7s\n", "JobAdd", "JobFin", "JobCancel", "SubAdd", "SubFin", "Locks", "Waits", "Kicks",
				   "JobAdd", "JobFin", "SubAdd", "SubFin", "Handles", "WrapTime", "Time", "Workers");
		}

		uint64_t nDelta			 = JqTick() - g_LogTickLast;
		uint64_t nTicksPerSecond = JqTicksPerSecond();
		float	 fTime			 = 1000.f * nDelta / nTicksPerSecond;
		double	 HandlesPerMs	 = nHandleConsumption / fTime;
		double	 HandlesPerYear	 = (0x8000000000000000 / (365llu * 24 * 60 * 60 * 60 * 1000)) / HandlesPerMs;
		(void)HandlesPerYear;

		double WrapTime = (uint64_t)0x8000000000000000 / (nHandleConsumption ? nHandleConsumption : 1) * (1.0 / (365 * 60.0 * 60.0 * 60.0 * 24.0));
		(void)WrapTime;
		printf("%c|        %10.2f/%10.2f/%10.2f, %10.2f/%10.2f|%8.2f %8.2f %8.2f|      %8d/%8d, %14d/%14d|%8lld|%12.2fy|%6.2fs|%2d     ", bUseWrapping ? '\r' : ' ', Stats.nNumAdded / (float)fTime,
			   Stats.nNumFinished / (float)fTime, Stats.nNumCancelled / (float)fTime, Stats.nNumAddedSub / (float)fTime, Stats.nNumFinishedSub / (float)fTime, Stats.nNumLocks / (float)fTime,
			   Stats.nNumWaitCond / (float)fTime, Stats.nNumWaitKicks / (float)fTime, g_LogStats.nNumAdded, g_LogStats.nNumFinished, g_LogStats.nNumAddedSub, g_LogStats.nNumFinishedSub,
			   nHandleConsumption, HandlesPerYear, fTime / 1000.f, JqGetNumWorkers());
		fflush(stdout);

		Frames		  = 0;
		g_LogTickLast = JqTick();
	}
}