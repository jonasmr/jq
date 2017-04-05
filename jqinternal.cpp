#include "jq.h"
#include "jqinternal.h"

#if JQ_LOCK_STATS
std::atomic<uint32_t> g_JqLockOps;
std::atomic<uint32_t> g_JqCondWait;
std::atomic<uint32_t> g_JqCondSignal;
std::atomic<uint32_t> g_JqSemaSignal;
std::atomic<uint32_t> g_JqSemaWait;
std::atomic<uint32_t> g_JqLocklessPops;
#endif

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
	JQLSC(g_JqLockOps.fetch_add(1));
}

void JqMutex::Unlock()
{
	LeaveCriticalSection(&CriticalSection);
	JQLSC(g_JqLockOps.fetch_add(1));
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
	JQLSC(g_JqCondWait.fetch_add(1));
	SleepConditionVariableCS(&Cond, &Mutex.CriticalSection, INFINITE);
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
	Handle = CreateSemaphoreEx(NULL, 0, nCount*2, NULL, 0, SEMAPHORE_ALL_ACCESS);	
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

#if defined(__APPLE__)
JqMutex::JqMutex()
{
	UnfairLock = OS_UNFAIR_LOCK_INIT;
}

JqMutex::~JqMutex()
{
}

void JqMutex::Lock()
{
	uint64_t tid;
	pthread_threadid_np(NULL, &tid);
	JQ_ASSERT(tid != 0);
	if(Owner == tid)
	{
		Count++;
	}
	else
	{
		os_unfair_lock_lock(&UnfairLock);
		JQ_ASSERT(Owner == 0);
		JQ_ASSERT(Count == 0);
		Count++;
		Owner = tid;
	}
	// pthread_mutex_lock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));

}

void JqMutex::Unlock()
{
	uint64_t tid;
	pthread_threadid_np(NULL, &tid);
	JQ_ASSERT(Owner == tid);
	Count--;
	if(0 == Count)
	{
		Owner = 0;
		os_unfair_lock_unlock(&UnfairLock);
	}

	JQLSC(g_JqLockOps.fetch_add(1));
}

JqCondMutex::JqCondMutex()
{
	pthread_mutex_init(&Mutex, 0);
}

JqCondMutex::~JqCondMutex()
{
	pthread_mutex_destroy(&Mutex);
}

void JqCondMutex::Lock()
{
	pthread_mutex_lock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));

}

void JqCondMutex::Unlock()
{
	pthread_mutex_unlock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));
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
	JQLSC(g_JqLockOps.fetch_add(1));

}

void JqMutex::Unlock()
{
	pthread_mutex_unlock(&Mutex);
	JQLSC(g_JqLockOps.fetch_add(1));
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

void JqConditionVariable::Wait(JqCondMutex& Mutex)
{
	pthread_cond_wait(&Cond, &Mutex.Mutex);

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
		JqCondMutexLock l(Mutex);
		uint32_t nCurrent = nReleaseCount.load();
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
	JqCondMutexLock l(Mutex);
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
	int nPageSize = sysconf(_SC_PAGE_SIZE);
	int nSize = (nStackSize + nPageSize - 1) & ~(nPageSize-1);
	void* pAlloc = mmap(nullptr, nSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
	return pAlloc;
}

void JqFreeStackInternal(void* p, uint32_t nStackSize)
{
	munmap(p, nStackSize);
}
#endif
#endif


JqJobStack* JqAllocStack(JqJobStackList& FreeList, uint32_t nStackSize, uint32_t nFlags)
{
	do{
		JqJobStackLink Value = FreeList.load();
		JqJobStack* pHead = Value.pHead;

		if(!pHead)
			break;

		JqJobStack* pNext = pHead->pLink;
		JqJobStackLink NewValue = {pNext, Value.nCounter+1 };
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
			JQ_ASSERT((nFlags&JQ_JOBFLAG_LARGE_STACK) == (pHead->nFlags&JQ_JOBFLAG_LARGE_STACK));
			pHead->pLink = nullptr;
			return pHead;
		}
	}while(1);

#ifdef JQ_MICROPROFILE
	MICROPROFILE_COUNTER_ADD("jq/stack/count", 1);
	MICROPROFILE_COUNTER_ADD("jq/stack/bytes", nStackSize);
#endif
	void* pStack = JqAllocStackInternal(nStackSize);
	JqJobStack* pJobStack = JqJobStack::Init(pStack, nStackSize, nFlags&JQ_JOBFLAG_LARGE_STACK);
	return pJobStack;
}
void JqFreeAllStacks(JqJobStackList& FreeList)
{
	do
	{
		JqJobStackLink Value = FreeList.load();
		JqJobStack* pHead = Value.pHead;
		if(!pHead)
		{
			return;
		}
		JqJobStackLink NewValue = {pHead->pLink, Value.nCounter + 1};
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
#ifdef JQ_MICROPROFILE
			MICROPROFILE_COUNTER_SUB("jq/stack/count", 1);
			MICROPROFILE_COUNTER_SUB("jq/stack/bytes", pHead->nStackSize);
#endif
			JqFreeStackInternal(pHead->StackBottom(), pHead->nStackSize);
		}

	}while(1);
}

void JqFreeStack(JqJobStackList& FreeList, JqJobStack* pStack)
{
	JQ_ASSERT(pStack->pLink == nullptr);
	do
	{
		JqJobStackLink Value = FreeList.load();
		JqJobStack* pHead = Value.pHead;
		pStack->pLink = pHead;
		JqJobStackLink NewValue = {pStack, Value.nCounter + 1};
		if(FreeList.compare_exchange_strong(Value, NewValue))
		{
			return;
		}

	}while(1);
}


void JqInitAttributes(JqAttributes* pAttributes, uint32_t nNumWorkers)
{
	JQ_ASSERT(nNumWorkers <= JQ_MAX_THREADS);
	memset(pAttributes, 0, sizeof(*pAttributes));
	pAttributes->nNumWorkers = nNumWorkers;
	pAttributes->nStackSizeSmall = JQ_DEFAULT_STACKSIZE_SMALL;
	pAttributes->nStackSizeLarge = JQ_DEFAULT_STACKSIZE_LARGE;
	for(uint32_t i = 0; i < nNumWorkers; ++i)
	{
		JqThreadConfig& C = pAttributes->ThreadConfig[i];
		memset(&C.nPipes[0], 0xff, sizeof(C.nPipes));
		for(uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
		{
			C.nNumPipes = JQ_NUM_PIPES;
			C.nPipes[j] = (uint8_t)j;
		}
	}
}

void JqStart(int nNumWorkers)
{
	JqAttributes Attr;
	JqInitAttributes(&Attr, nNumWorkers);
	JqStart(&Attr);
}





