#include "jq.h"
#include "jqinternal.h"
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
	if(nReleaseCount.load() == nMaxCount)
	{
		return;
	}
	{
		JqMutexLock l(Mutex);
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
	JQ_MICROPROFILE_SCOPE("Wait", 0xc0c0c0);
	JqMutexLock l(Mutex);
	while(!nReleaseCount)
	{
		Cond.Wait(Mutex);
	}
	nReleaseCount--;
}

#endif




#ifdef _WIN32
void* JqAllocStackInternal(uint32_t nStackSize)
{
	JQ_BREAK();
}
void JqFreeStackInternal(void* pStack, uint32_t nStackSize)
{
	//never called
	JQ_BREAK();
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
	//never called.
	munmap(p, nStackSize);
}
#endif


JqJobStack* JqAllocStack(JqJobStackList& FreeList, uint32_t nFlags)
{
	bool bSmall = 0 == (nFlags&JQ_JOBFLAG_LARGE_STACK);
	uint32_t nStackSize = bSmall ? JQ_STACKSIZE_SMALL : JQ_STACKSIZE_LARGE;
	// auto& FreeList = bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
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
	if(bSmall)
	{
		MICROPROFILE_COUNTER_ADD("jq/stack/small/count", 1);
		MICROPROFILE_COUNTER_ADD("jq/stack/small/bytes", nStackSize);
	}
	else
	{
		MICROPROFILE_COUNTER_ADD("jq/stack/large/count", 1);
		MICROPROFILE_COUNTER_ADD("jq/stack/large/bytes", nStackSize);
	}
#endif
	void* pStack = JqAllocStackInternal(nStackSize);
	JqJobStack* pJobStack = JqJobStack::Init(pStack, nStackSize, nFlags&JQ_JOBFLAG_LARGE_STACK);
	return pJobStack;
}

void JqFreeStack(JqJobStackList& FreeList, JqJobStack* pStack)
{
	// bool bSmall = 0 == (nFlags&JQ_JOBFLAG_LARGE_STACK);
	// uint32_t nStackSize = bSmall ? JQ_STACKSIZE_SMALL : JQ_STACKSIZE_LARGE;
	// (void)nStackSize;

	// auto& FreeList = bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
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








