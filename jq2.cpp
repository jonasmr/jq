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
#ifdef JQ_USE_LOCKLESS

#include <stddef.h>
#include <stdint.h>

struct JqJobStack;

#include "jqbase.h"
#include "jqinternal.h"
#ifdef JQ_MICROPROFILE
#include "microprofile.h"
#endif
#include "jqfcontext.h"


struct JqStatsInternal
{
	std::atomic<uint32_t> nNumAdded;
	std::atomic<uint32_t> nNumFinished;
	std::atomic<uint32_t> nNumAddedSub;
	std::atomic<uint32_t> nNumFinishedSub;
	std::atomic<uint32_t> nNumCancelled;
	std::atomic<uint32_t> nNumCancelledSub;
	std::atomic<uint32_t> nNumLocks;
	std::atomic<uint32_t> nNumWaitKicks;
	std::atomic<uint32_t> nNumWaitCond;
	std::atomic<uint32_t> nMemoryUsed;
	std::atomic<uint32_t> nAttempts;
	std::atomic<uint32_t> nNextHandleCalled;
	std::atomic<uint32_t> nSkips;
};



#define JQ_NUM_JOBS (JQ_TREE_BUFFER_SIZE2-1)
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
void 		JqWorker(int nThreadId);
uint64_t 	JqNextHandle();
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


#define JQ_TREE_BUFFER_SIZE2 (1<<JQ_PIPE_EXTERNAL_ID_BITS) // note: this should match with nExternalId size in JqPipe

struct JqJobTreeState
{
	uint16_t nParent;
	uint16_t nFirstChild;
	uint16_t nSibling;
	uint8_t nDeleted;
	uint8_t nDeleteCounter;
};
struct JqJobFinish
{
	uint64_t nStarted;
	uint64_t nFinished;
};
#include "jqpipe.h"
struct JqJob2
{
	JqFunction Function;

	std::atomic<JqJobFinish> 	Handle;
	std::atomic<JqPipeHandle> 	PipeHandle;
	std::atomic<uint64_t> 		nRoot;
	uint32_t 					nJobFlags;
	int Reserved;
	enum
	{
		EJOBTREE_NULL = 0,
		EJOBTREE_PENDING_REMOVAL = 0xffffffff,
	};

	std::atomic<JqJobTreeState> TreeState;
	//the n-tree insertion is possible because:
	//its guaranteed that 
	//   	when inserting a child, parent will never be removed or disappear - This is because childs
	//		when removing a child it has no childs itself.
	//it then is reduced to lockless single list insertion & removal.
};

#define JQ_PAD_SIZE(type) (JQ_CACHE_LINE_SIZE - (sizeof(type)%JQ_CACHE_LINE_SIZE))
#ifdef _WIN32
#define JQ_ALIGN_CACHELINE __declspec(align(JQ_CACHE_LINE_SIZE))
#define JQ_ALIGN_16 __declspec(align(16))
#else
#define JQ_ALIGN_CACHELINE __attribute__((__aligned__(JQ_CACHE_LINE_SIZE)))
#define JQ_ALIGN_16 __attribute__((__aligned__(16)))
#endif


#define JQ_STATS(a) do{a;}while(0)

#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS //note: This is just arbitrary: given a suffiently random priority setup you might need to bump this.

struct JqJobStackLink
{
	JqJobStack* pHead;
	uint32_t 	nCounter;
};

struct JQ_ALIGN_CACHELINE JqState_t
{
	struct JQ_ALIGN_CACHELINE JqPaddedSemaphore
	{
		JqSemaphore S;
		char pad0[JQ_PAD_SIZE(JqSemaphore)];
	};
	JqPaddedSemaphore Semaphore[JQ_MAX_SEMAPHORES];

	JqMutex Mutex;
	char pad1[ JQ_PAD_SIZE(JqMutex) ]; 

	JqConditionVariable WaitCond;
	char pad2[ JQ_PAD_SIZE(JqConditionVariable) ];


	uint64_t m_SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t m_PipeNumSemaphores[JQ_NUM_PIPES];
	uint8_t m_PipeToSemaphore[JQ_NUM_PIPES][JQ_MAX_SEMAPHORES];
	uint8_t m_SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t m_SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int		m_ActiveSemaphores;


	JQ_THREAD WorkerThreads[JQ_MAX_THREADS];

	JQ_THREAD SentinelThread;
	int nNumWorkers;
	int nStop;
	int nStopSentinel;
	int nTotalWaiting;
	int nSentinelRunning;
	std::atomic<int> nFrozen;
	std::atomic<uint64_t> nNextHandle;
	uint32_t nFreeJobs;
	uint64_t nShortOnlyMask;


	std::atomic<uint64_t> 	nNumFinished;
	std::atomic<uint64_t> 	nNumAdded;

	uint8_t m_nNumPipes[JQ_MAX_THREADS];
	uint8_t m_PipeList[JQ_MAX_THREADS][JQ_NUM_PIPES];
	uint8_t m_SemaphoreIndex[JQ_MAX_THREADS];


	JqPipe 			m_Pipes[JQ_NUM_PIPES];
	JqJob2 			m_Jobs2[JQ_TREE_BUFFER_SIZE2];

	std::atomic<JqJobStackLink> m_StackSmall;
	std::atomic<JqJobStackLink> m_StackLarge;
	

	JqState_t()
		:nNumWorkers(0)
	{
	}


	JqStatsInternal Stats;
} JqState;


struct JqJobStack
{
	uint64_t GUARD[2];
	JqFContext ContextReturn;
	JqFContext pContextJob;

	JqJobStack* pLink;//when in use: Previous. When freed, next element in free list

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
		pJobStack->GUARD[0] = 0xececececececececll;
		pJobStack->GUARD[1] = 0xececececececececll;
		return pJobStack;
	}
};

JQ_THREAD_LOCAL JqJobStack* g_pJqJobStacks = 0;



#ifdef _WIN32
void* JqAllocStackInternal(uint32_t nStackSize)
{
	JQ_BREAK();
}
void JqFreeStackInternal(void* pStack)
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

JqJobStack* JqAllocStack(uint32_t nFlags)
{
	bool bSmall = 0 == (nFlags&JQ_JOBFLAG_LARGE_STACK);
	uint32_t nStackSize = bSmall ? JQ_STACKSIZE_SMALL : JQ_STACKSIZE_LARGE;
	auto& FreeList = bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
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

void JqFreeStack(JqJobStack* pStack, uint32_t nFlags)
{
	bool bSmall = 0 == (nFlags&JQ_JOBFLAG_LARGE_STACK);
	uint32_t nStackSize = bSmall ? JQ_STACKSIZE_SMALL : JQ_STACKSIZE_LARGE;
	(void)nStackSize;

	auto& FreeList = bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
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

void JqContextRun(JqTransfer T)
{
	JqJobStack * pJobData = (JqJobStack*)T.data;
	JqState.m_Jobs2[pJobData->nExternalId].Function(pJobData->nBegin, pJobData->nEnd);
	jump_fcontext(T.fctx, (void*)447);
	JQ_BREAK();
}


void JqRunInternal(uint32_t nExternalId, int nBegin, int nEnd)
{
#if JQ_USE_SEPERATE_STACK
	{
		{
			uint32_t nFlags = JqState.m_Jobs2[nExternalId].nJobFlags;
			JqJobStack* pJobData = JqAllocStack(nFlags);
			void* pHest = g_pJqJobStacks;
			JQ_ASSERT(pJobData->pLink == nullptr);
			pJobData->pLink = g_pJqJobStacks;
			pJobData->nBegin = nBegin;
			pJobData->nEnd = nEnd;
			pJobData->nExternalId = nExternalId;
			g_pJqJobStacks = pJobData;
			pJobData->pContextJob = make_fcontext( pJobData->StackTop(), pJobData->StackSize(), JqContextRun);
			JqTransfer T = jump_fcontext(pJobData->pContextJob, (void*) pJobData);
			JQ_ASSERT(T.data == (void*)447);
			g_pJqJobStacks = pJobData->pLink;
			pJobData->pLink = nullptr;
			JQ_ASSERT(pHest == g_pJqJobStacks);
			JQ_ASSERT(pJobData->GUARD[0] == 0xececececececececll);
			JQ_ASSERT(pJobData->GUARD[1] == 0xececececececececll);
			JqFreeStack(pJobData, nFlags);
		}
	}
#else
	JqState.m_Jobs2[nExternalId].Function(nBegin, nEnd);
#endif
}



void JqCheckFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	JQ_ASSERT(nIndex < JQ_TREE_BUFFER_SIZE2);
	JqPipeHandle PipeHandle = JqState.m_Jobs2[nIndex].PipeHandle.load();
	JqJobTreeState State = JqState.m_Jobs2[nIndex].TreeState.load(std::memory_order_acquire);
	if(0 == State.nFirstChild && 0 == PipeHandle.Handle && PipeHandle.Invalid == 0 && JqState.m_Jobs2[nIndex].Handle.load().nStarted == nJob)
	{
		uint16_t nParent = State.nParent;
		uint64_t nParentHandle = 0;
		//JQ_CLEAR_FUNCTION(JqState.m_Jobs2[nIndex].Function);
		if(nParent)
		{
			nParentHandle = JqDetachChild(nJob);
		}
		JqJobFinish Finish;
		JqJobFinish FinishNew;
		bool bFail = false;
		do
		{
			Finish = JqState.m_Jobs2[nIndex].Handle.load();
			if(Finish.nStarted != nJob || Finish.nFinished == nJob)
			{
				JQ_ASSERT(JQ_LE_WRAP(nJob, Finish.nStarted));
				bFail = true;
				break;
			}
			JQ_ASSERT(Finish.nFinished != nJob);
			FinishNew = Finish;
			FinishNew.nFinished = nJob;
		}while(!JqState.m_Jobs2[nIndex].Handle.compare_exchange_weak(Finish, FinishNew));

		if(!bFail)
		{
			JqState.nFreeJobs++;
		}
		if(nParentHandle)
		{
			JqCheckFinished(nParentHandle);
		}
	}
}

uint32_t g_TESTID = 0;
void JqFinishJobHelper(JqPipeHandle PipeHandle, uint32_t nExternalId, int nNumJobs, int nCancel)
{
	JQ_ASSERT(nExternalId != 0);
	JQ_ASSERT(nExternalId < JQ_TREE_BUFFER_SIZE2);
	JqPipeHandle Null = JqPipeHandleNull();
	JqPipeHandle Current = JqState.m_Jobs2[nExternalId].PipeHandle.load(std::memory_order_acquire);
	if(nCancel)
	{
		uint64_t nHandle = JqState.m_Jobs2[nExternalId].Handle.load().nStarted;
		if(0 != Current.Handle && JqState.m_Jobs2[nExternalId].PipeHandle.compare_exchange_weak(Current, Null))
		{
			JqState.Stats.nNumCancelledSub += nNumJobs;
			JqState.Stats.nNumCancelled++;
			JqCheckFinished(nHandle);		}
		else
		{
			JQ_BREAK(); //should never happen.
		}
	}
	else
	{
		uint64_t nHandle = JqState.m_Jobs2[nExternalId].Handle.load().nStarted;
		if(0 != Current.Handle && JqState.m_Jobs2[nExternalId].PipeHandle.compare_exchange_weak(Current, Null))
		{
			if(nExternalId == g_TESTID)
			{
				printf("HERE!!\n");
			}
			JqState.Stats.nNumFinishedSub += nNumJobs;
			JqState.Stats.nNumFinished++;
			JqState.nNumFinished.fetch_add(1);
			JqCheckFinished(nHandle);
		}
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
	uint64_t nHandle = JqState.m_Jobs2[nExternalId].Handle.load().nStarted;
	JqSelfPush(nHandle, nSubIndex);
	//uint32_t nHandleInternal = PipeHandle.nHandleInternal;
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;	
	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
	JqRunInternal(nExternalId, nStart, nEnd);
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

	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1)&(uint64_t)&JqState) == 0);
	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1)&offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.nNumWorkers == 0);
	memset(JqState.m_PipeList, 0xff, sizeof(JqState.m_PipeList));
	memset(JqState.m_nNumPipes, 0, sizeof(JqState.m_nNumPipes));
	memset(JqState.m_Pipes, 0, sizeof(JqState.m_Pipes));
	memset(JqState.m_Jobs2, 0, sizeof(JqState.m_Jobs2));

	memset(JqState.m_SemaphoreMask, 0, sizeof(JqState.m_SemaphoreMask));
	memset(JqState.m_PipeNumSemaphores, 0, sizeof(JqState.m_PipeNumSemaphores));
	memset(JqState.m_PipeToSemaphore, 0, sizeof(JqState.m_PipeToSemaphore));
	memset(JqState.m_SemaphoreClients, 0, sizeof(JqState.m_SemaphoreClients));
	memset(JqState.m_SemaphoreClientCount, 0, sizeof(JqState.m_SemaphoreClientCount));	
	JqState.m_ActiveSemaphores = 0;


	for(int i = 0; i < nNumWorkers; ++i)
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
		for(int j = 0; j < JqState.m_ActiveSemaphores; ++j)
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
	for (uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
	{
		JqState.m_Pipes[i].nPut.store(1);
		JqState.m_Pipes[i].nGet.store(1);
		JqState.m_Pipes[i].StartJobFunc = JqRunJobHelper;
		JqState.m_Pipes[i].FinishJobFunc = JqFinishJobHelper;
		JqState.m_Pipes[i].nPipeId = (uint8_t)i;
	}
	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		JqState.Semaphore[i].S.Init(JqState.m_SemaphoreClientCount[i] ? JqState.m_SemaphoreClientCount[i] : 1);
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
	JqState.nStopSentinel = 0;
	JqState.nSentinelRunning = 0;
	JqState.nTotalWaiting = 0;

	JqState.nNextHandle = 1;
	JqJobFinish F;
	F.nStarted = JqState.nNextHandle-1;
	F.nFinished = JqState.nNextHandle-1;

	for(uint32_t i = 0; i < JQ_TREE_BUFFER_SIZE2; ++i)
	{
		JqState.m_Jobs2[i].Handle.store(F);
	}
	for(int i = 0; i < nNumWorkers; ++i)
	{
		JQ_THREAD_CREATE(&JqState.WorkerThreads[i]);
		JQ_THREAD_START(&JqState.WorkerThreads[i], JqWorker, (i));
	}
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
		JqState.Semaphore[i].S.Signal(JqState.nNumWorkers);
	}
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
	//JqMutexLock lock(JqState.Mutex);
	pStats->nNumAdded = JqState.Stats.nNumAdded.exchange(0);
	pStats->nNumFinished = JqState.Stats.nNumFinished.exchange(0);
	pStats->nNumAddedSub = JqState.Stats.nNumAddedSub.exchange(0);
	pStats->nNumFinishedSub = JqState.Stats.nNumFinishedSub.exchange(0);
	pStats->nNumCancelled = JqState.Stats.nNumCancelled.exchange(0);
	pStats->nNumCancelledSub = JqState.Stats.nNumCancelledSub.exchange(0);
	pStats->nNumLocks = JqState.Stats.nNumLocks.exchange(0);
	pStats->nNumWaitKicks = JqState.Stats.nNumWaitKicks.exchange(0);
	pStats->nNumWaitCond = JqState.Stats.nNumWaitCond.exchange(0);
	pStats->nNextHandle = JqState.nNextHandle.load();
	pStats->nAttempts = JqState.Stats.nAttempts.exchange(0);
	pStats->nSkips = JqState.Stats.nSkips.exchange(0);
	pStats->nNextHandleCalled = JqState.Stats.nNextHandleCalled.exchange(0);
}



void JqAttachChild(uint64_t nParent, uint64_t nChild)
{
	uint16_t nParentIndex = nParent % JQ_TREE_BUFFER_SIZE2;
	uint16_t nChildIndex = nChild % JQ_TREE_BUFFER_SIZE2;
	JqJob2* pChild = &JqState.m_Jobs2[nChildIndex];
	JqJob2* pParent = &JqState.m_Jobs2[nParentIndex];

	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].Handle.load().nStarted == nChild);
	JQ_ASSERT(JqState.m_Jobs2[nChildIndex].Handle.load().nFinished != nChild);
	JQ_ASSERT(JqState.m_Jobs2[nParentIndex].Handle.load().nStarted == nParent);
	JQ_ASSERT(JqState.m_Jobs2[nParentIndex].Handle.load().nFinished != nParent);

	JqJobTreeState ChildState = pChild->TreeState.load();
	JQ_ASSERT(ChildState.nSibling == 0);
	JQ_ASSERT(ChildState.nFirstChild == 0);
	JQ_ASSERT(ChildState.nParent == 0);
	ChildState.nParent = nParentIndex;
	JqJobTreeState ParentState, NewParentState;
	do
	{
		ParentState = pParent->TreeState.load(std::memory_order_acquire);
		ChildState.nSibling = ParentState.nFirstChild;
		JQ_ASSERT(ChildState.nDeleted == 0);
		JQ_ASSERT(ParentState.nDeleted == 0);
		pChild->TreeState.store(ChildState, std::memory_order_release);
		NewParentState = ParentState;
		NewParentState.nFirstChild = nChildIndex;

	}while(!pParent->TreeState.compare_exchange_weak(ParentState, NewParentState));
	pChild->nRoot.store(pParent->nRoot.load());
}

uint64_t JqDetachChild(uint64_t nChild)
{
	uint16_t nChildIndex = nChild % JQ_TREE_BUFFER_SIZE2;
	JqJob2* pChild = &JqState.m_Jobs2[nChildIndex];
	JqJobTreeState BaseState = pChild->TreeState.load(std::memory_order_acquire);
	uint16_t nParentIndex = BaseState.nParent;
	JqJob2* pParent = &JqState.m_Jobs2[nParentIndex];
	JQ_ASSERT(pChild->PipeHandle.load().Handle == 0); //hmmmmmmmmm does this make sense? verify

	JQ_ASSERT(BaseState.nFirstChild == 0);
	if(0 == nParentIndex)
	{
		JQ_ASSERT(BaseState.nSibling == 0);
		return 0;
	}
	//tag as deleted --noget fucker up når den kommer ind her efter den er slettet..
	JqJobTreeState State, NewState;
	uint64_t nParentHandle = 0;
	do
	{
		State = pChild->TreeState.load(std::memory_order_acquire);
		nParentHandle = JqState.m_Jobs2[State.nParent % JQ_TREE_BUFFER_SIZE2 ].Handle.load().nStarted;
		JQ_ASSERT(JQ_LE_WRAP(JqState.m_Jobs2[State.nParent % JQ_TREE_BUFFER_SIZE2].Handle.load().nStarted, nParentHandle));
		if(State.nDeleted || 0 == State.nParent)
			break;
		NewState = State;
		NewState.nDeleted = 1;
	}while(!pChild->TreeState.compare_exchange_weak(State, NewState));

	JqJobTreeState ChildState = pChild->TreeState.load(std::memory_order_acquire);

	while(ChildState.nDeleted != 0)
	{
		JQ_ASSERT(ChildState.nParent);
		uint16_t nPrevIndex = 0;
		uint16_t nDeleteIndex = 0;
		JqJobTreeState PrevState, NewState, DeleteState,xx;
		do
		{

			PrevState = pParent->TreeState.load();
			nPrevIndex = nParentIndex;
			nDeleteIndex = PrevState.nFirstChild;
			DeleteState = JqState.m_Jobs2[nDeleteIndex].TreeState.load();

			while(!DeleteState.nDeleted)
			{
				nPrevIndex = nDeleteIndex;
				nDeleteIndex = DeleteState.nSibling;
				if(!nDeleteIndex)
				{
					break;
				}
				PrevState = JqState.m_Jobs2[nPrevIndex].TreeState.load();
				DeleteState = JqState.m_Jobs2[nDeleteIndex].TreeState.load();

			}
			if(PrevState.nDeleted)
			{
				nDeleteIndex = 0;
			}
			if(!nDeleteIndex)
			{
				break;
			}
			//delete
			JQ_ASSERT(DeleteState.nDeleted);
			JQ_ASSERT(!PrevState.nDeleted);
			NewState = PrevState; 
			if(PrevState.nFirstChild == nDeleteIndex)
			{
				NewState.nFirstChild = DeleteState.nSibling;
			}
			else
			{
				if(PrevState.nSibling != nDeleteIndex || PrevState.nParent != nParentIndex)
				{
					nDeleteIndex = 0;
					break;
				}
				JQ_ASSERT(PrevState.nSibling == nDeleteIndex);
				NewState.nSibling = DeleteState.nSibling;
			}
			if(0 == memcmp(&NewState, &PrevState, sizeof(NewState)))
			{
				nDeleteIndex = 0;
				break;
			}
			xx = PrevState;
			//JQ_ASSERT(xx.nParent == NewState.nParent);
		}while(!JqState.m_Jobs2[nPrevIndex].TreeState.compare_exchange_weak(PrevState, NewState));
		if(nDeleteIndex)
		{
			JqJobFinish F = JqState.m_Jobs2[nDeleteIndex].Handle.load();
			JQ_ASSERT(F.nFinished != F.nStarted);
			JqJobTreeState S = JqState.m_Jobs2[nDeleteIndex].TreeState.load();
			int cmp = memcmp(&NewState, &xx, sizeof(NewState));
			JQ_ASSERT(0 != cmp);
			JQ_ASSERT(S.nDeleted == 1);
			S.nParent = 0;
			S.nSibling = 0;
			S.nDeleted = 0;
			S.nDeleteCounter++;
			JQ_ASSERT(S.nFirstChild == 0);
			JqState.m_Jobs2[nDeleteIndex].TreeState.store(S);
			// if(nDeleteIndex == 53 || nDeleteIndex==52)
			// {
			// 	printf("delete %d\n", nDeleteIndex);
			// }
		}
		//her .. tjek delete i add xxxx 
		ChildState = pChild->TreeState.load(std::memory_order_acquire);
	}
	ChildState = pChild->TreeState.load(std::memory_order_acquire);
	JQ_ASSERT(ChildState.nFirstChild == 0);
	JQ_ASSERT(ChildState.nSibling == 0);
	JQ_ASSERT(ChildState.nDeleted == 0);
	JQ_ASSERT(ChildState.nParent == 0);
	JQ_ASSERT( JqState.m_Jobs2[nParentHandle%JQ_TREE_BUFFER_SIZE2].TreeState.load().nFirstChild != nChildIndex);

	return nParentHandle;
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


#define JS(i) JqState.m_Jobs2[i].TreeState.load()


//Depth first. Once a node is visited all child nodes have been visited
uint16_t JqTreeIterate(uint64_t nJob, uint16_t nCurrent)
{
	JQ_ASSERT(nJob);
	uint16_t nRoot = nJob % JQ_TREE_BUFFER_SIZE2;
	if(JqState.m_Jobs2[nRoot].nRoot != JqState.m_Jobs2[nCurrent].nRoot)
	{
		return 0; //not root, restart iteration
	}
	if(nRoot == nCurrent)
	{
		while(JS(nCurrent).nFirstChild)
			nCurrent = JS(nCurrent).nFirstChild;
	}
	else
	{
		//once here all child nodes _have_ been processed.
		if(JS(nCurrent).nSibling)
		{
			nCurrent = JS(nCurrent).nSibling;
			while(JS(nCurrent).nFirstChild) //child nodes first.
				nCurrent = JS(nCurrent).nFirstChild;
		}
		else
		{
			nCurrent = JS(nCurrent).nParent;
		}
	}
	return nCurrent;
}

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
	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint64_t nRootHandle = JqState.m_Jobs2[nIndex].nRoot.load();
	uint16_t nRootIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint32_t nIdleRepeat = 0;

	if(JqIsDone(nJob))
		return false;
	
	do
	{
		if(JqIsDone(nJob))
			return false;
		nIndex = JqTreeIterate(nJob, nIndex);
		if(!nIndex)
		{
			nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
			++nIdleRepeat;
			if(nIdleRepeat > 10000)
			{
				JQ_ASSERT(0);
				nIdleRepeat = 0;
			}
			continue;
		}
		else
		{
			nIdleRepeat = 0;
		}
		JQ_ASSERT(nIndex);
		JqPipeHandle ChildHandle = JqState.m_Jobs2[nIndex].PipeHandle.load();
		uint64_t nChildRoot = JqState.m_Jobs2[nIndex].nRoot.load();
		if(nChildRoot == nRootHandle)
		{
			if(JqPipeExecute(&JqState.m_Pipes[ChildHandle.Pipe], ChildHandle))
			{
				return true;
			}
		}

	}while(nIndex != nRootIndex);
	return false;
}





void JqWorker(int nThreadId)
{
	uint8_t* pPipes = JqState.m_PipeList[nThreadId];
	uint32_t nNumPipes = JqState.m_nNumPipes[nThreadId];
	g_nJqNumPipes = nNumPipes; //even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqPipes, pPipes, nNumPipes);
	int nSemaphoreIndex = JqState.m_SemaphoreIndex[nThreadId];
#if JQ_MICROPROFILE
	char PipeStr[512];
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
			JqState.Semaphore[nSemaphoreIndex].S.Wait();
		}
	}
#if MICROPROFILE_ENABLED
	MicroProfileOnThreadExit();
#endif
}


void JqDump(FILE* F, uint32_t nNode, uint32_t nIndent)
{
	JqJob2* pJob = &JqState.m_Jobs2[nNode];
	for(uint32_t i = 0; i < nIndent; ++i )
		fprintf(F, "  ");
	JqJobTreeState TS = pJob->TreeState.load();
	fprintf(F, "%08d %lld/%lld, root %lld pipe %lld, pipehandle %lld  PCS, %5d, %5d, %5d\n", nNode, pJob->Handle.load().nStarted, pJob->Handle.load().nFinished, pJob->nRoot.load(), pJob->PipeHandle.load().Pipe, pJob->PipeHandle.load().HandleInt, 
		TS.nParent,
		TS.nFirstChild,
		TS.nSibling

		);
}

void JqDumpTree(FILE* F, uint32_t nTree, uint32_t nDepth)
{
	JqDump(F, nTree, nDepth);
	JqJob2* pJob = &JqState.m_Jobs2[nTree];
	uint16_t nChild = pJob->TreeState.load().nFirstChild;
	while(nChild)
	{
		JqDumpTree(F, nChild, nDepth + 1);
		nChild = JqState.m_Jobs2[nChild].TreeState.load().nSibling;
	}
}
void JqDump()
{
	JqState.nFrozen.store(1);
	JQ_USLEEP(1000*1000);
	FILE* F = fopen("jqdump.txt", "w");
	printf("dumping\n");
	fprintf(F, "JQ DUMP\n");
	fprintf(F, "num finished jobs %d\n", JqState.Stats.nNumFinished.load());

	uint32_t nHandleUnfinished = 0;
	uint32_t nPipeHandleUnfinished = 0;
	uint32_t nPipes[JQ_NUM_PIPES] = {0};
	uint32_t nHasRoot = 0;
	uint32_t nInTree = 0;
	uint32_t nHasParent = 0;
	uint32_t nHasChild = 0;
	uint32_t nHasSibling = 0;
	uint32_t nDeleted = 0;

	for(uint32_t i = 1; i < JQ_TREE_BUFFER_SIZE2; ++i)
	{
		JqJob2* pJob = &JqState.m_Jobs2[i];
		if(pJob->Handle.load().nStarted != pJob->Handle.load().nFinished)
		{
			nHandleUnfinished ++;
		}
		JqPipeHandle PH = pJob->PipeHandle.load();
		JqJobTreeState TreeState = pJob->TreeState.load();
		if(PH.HandleInt)
		{
			if(PH.Pipe >= JQ_NUM_PIPES)
			{
				JQ_BREAK();
			}
			nPipeHandleUnfinished++;
			nPipes[PH.Pipe]++;			
		}
		if(TreeState.nParent != 0 || TreeState.nFirstChild || TreeState.nSibling)
			nInTree++;
		if(TreeState.nParent)
			nHasParent++;
		if(TreeState.nFirstChild)
			nHasChild++;
		if(TreeState.nSibling)
			nHasSibling++;
		if(TreeState.nDeleted)
			nDeleted++;
	}

	fprintf(F, "%05d TOTAL\n", JQ_TREE_BUFFER_SIZE2);
	fprintf(F, "%05d Handle unfinished\n", nHandleUnfinished);
	for(uint32_t i = 0;i < JQ_NUM_PIPES; ++i)
	{
		fprintf(F, "%05d Handle unfinished PIPE %d\n", nPipes[i], i);
	}

	fprintf(F, "%05d nHasRoot\n", nHasRoot);
	fprintf(F, "%05d nInTree\n", nInTree);
	fprintf(F, "%05d nHasParent\n", nHasParent);
	fprintf(F, "%05d nHasChild\n", nHasChild);
	fprintf(F, "%05d nHasSibling\n", nHasSibling);
	fprintf(F, "%05d nDeleted\n", nDeleted);
	fprintf(F, "XXXXX TREE DUMP\n");

	for(uint32_t i = 1; i < JQ_TREE_BUFFER_SIZE2; ++i)
	{
		JqJob2* pJob = &JqState.m_Jobs2[i];
		if(pJob->Handle.load().nStarted != pJob->Handle.load().nFinished)
		{
			if(0 == pJob->TreeState.load().nParent)
			{
				fprintf(F, "root %d\n", i);
				JqDumpTree(F, i, 0);
			}
		}
	}

	fprintf(F, "YYYYY RAW DUMP\n");
	for(uint32_t i = 0; i < JQ_TREE_BUFFER_SIZE2; ++i)
	{
		JqDump(F, i, 0);
	}



	fclose(F);

	for(uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
	{
		char fname[128];
		snprintf(fname, sizeof(fname)-1, "jqpipe%d.txt", i);
		FILE* F = fopen(fname, "w");
		fprintf(F, " XXX PIPE DUMP %d XXX\n", i);
		JqPipeDump(F, &JqState.m_Pipes[i]);
		fclose(F);
	}
	printf("dump complete\n");

}
void JqCrashAndDump()
{
	JqDump();

	JQ_ASSERT(0);

}

void JqSentinel(int nSeconds)
{
	int64_t nTickLimit = JqTicksPerSecond() * nSeconds;
	int64_t nTicks = 0;
	int64_t nTickLast = JqTick();
	uint64_t nJobs = JqState.nNumFinished.load();
	while(0 == JqState.nStopSentinel)
	{
		JQ_USLEEP(50000);
		int64_t nJobsCur = JqState.nNumFinished.load();
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


JQ_API void	JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nWaitFlag)
{
	uint64_t nJob = JqAdd(JobFunc, nPrio, nNumJobs, nRange);
	JqWait(nJob, nWaitFlag);
}

uint64_t JqNextHandle()
{
	uint64_t nHandle, nOldHandle;
	int nSkips = 0;
	int nAttempts = 0;
	JqJob2* pJob = nullptr;
	do
	{
		nOldHandle = JqState.nNextHandle.load();
		nHandle = nOldHandle + 1;
		uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
		pJob = &JqState.m_Jobs2[nIndex];
		JqJobFinish Finish = pJob->Handle.load();
		while(nIndex == 0 || Finish.nStarted != Finish.nFinished)
		{
			nHandle++;
			nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
			pJob = &JqState.m_Jobs2[nIndex];
			Finish = pJob->Handle.load();
			nSkips++;
		}
		nAttempts++;
	}while(!JqState.nNextHandle.compare_exchange_strong(nOldHandle, nHandle));
	// TODO: What happens when we have two contesting for the same job slot?
	//		 can this happen?
	//
	// 	JqJobFinish FinishNew = Finish;
	// 	FinishNew.nStarted = nHandle;
	// 	if(pJob->Handle.compare_exchange_strong(Finish, FinishNew))
	// 	{
	// 		if(JqState.nNextHandle.compare_exchange_strong(nOldHandle, nHandle))
	// 		{
	// 			//success
	// 			break;
	// 		}
	// 		else
	// 		{
	// 			bool bRet = pJob->Handle.compare_exchange_strong(Finish, FinishNew); //someone else claimed the job.
	// 			JQ_ASSERT(bRet);//never fails.
	// 		}
	// 	}
	// }while(1);






	JqState.Stats.nAttempts.fetch_add(nAttempts);
	JqState.Stats.nSkips.fetch_add(nSkips);
	JqState.Stats.nNextHandleCalled.fetch_add(1);
	return nHandle;
}

void JqBlockWhileFrozen()
{
	while(JqState.nFrozen.load())
	{
		JQ_USLEEP(20000);
	}
}
bool JqCancel(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	if(JqIsDone(nJob))
	{
		return false;
	}
	JqPipeHandle PipeHandle = JqState.m_Jobs2[nIndex].PipeHandle.load();
	JqJobFinish Finish = JqState.m_Jobs2[nIndex].Handle.load();
	if(JQ_GE_WRAP(Finish.nFinished, nJob) || PipeHandle.HandleInt == 0)
	{
		return false;
	}

	if(JqPipeCancel(&JqState.m_Pipes[PipeHandle.Pipe], PipeHandle))
	{
		JqJobFinish Finish;
		Finish = JqState.m_Jobs2[nIndex].Handle.load();


		JQ_ASSERT(Finish.nStarted == nJob); //remove for reuse.
		JQ_ASSERT(Finish.nFinished == nJob);
		return true;

	}
	return false;
}


uint64_t JqAdd(JqFunction JobFunc, uint8_t nPipe, int nNumJobs, int nRange, uint32_t nJobFlags)
{
	uint64_t nParent = 0 != (nJobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf();
	JqBlockWhileFrozen();
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
		nHandle = JqNextHandle();
		uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
		JqJob2* pEntry = &JqState.m_Jobs2[nIndex];
		JQ_ASSERT(pEntry->PipeHandle.load().Handle == 0);
		JQ_ASSERT(pEntry->TreeState.load().nFirstChild == 0);
		JQ_ASSERT(pEntry->TreeState.load().nParent == 0);
		JQ_ASSERT(pEntry->TreeState.load().nSibling == 0);
		pEntry->Function = JobFunc;
		pEntry->nJobFlags = nJobFlags;
		JqJobFinish F = pEntry->Handle.load();
		JqJobFinish FOld = F;
		F.nStarted = nHandle;
		bool bSuccess = pEntry->Handle.compare_exchange_weak(FOld, F);
		JQ_ASSERT(bSuccess);

		uint64_t nParentHandle = nParent;
		if(nParent)
		{
			JqAttachChild(nParentHandle, nHandle);
		}
		else
		{
			pEntry->nRoot = nHandle;
		}
		JqPipeHandle Handle = JqPipeAdd(&JqState.m_Pipes[nPipe], (uint32_t)nHandle, nNumJobs, nRange);
		pEntry->PipeHandle.store(Handle, std::memory_order_release);
		JqState.nNumAdded.fetch_add(1);
		JqState.Stats.nNumAdded.fetch_add(1);
		JqState.Stats.nNumAddedSub += nNumJobs;
	}

	uint32_t nNumSema = JqState.m_PipeNumSemaphores[nPipe];
	for (uint32_t i = 0; i < nNumSema; ++i)
	{
		int nSemaIndex = JqState.m_PipeToSemaphore[nPipe][i];
		JqState.Semaphore[nSemaIndex].S.Signal(nNumJobs);
	}
	return nHandle;
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	JqJobFinish F = JqState.m_Jobs2[nIndex].Handle.load();
	uint64_t nFinishedHandle = F.nFinished;
	uint64_t nStartedHandle = F.nStarted;
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

void JqWaitAll()
{
	while(JqState.nNumAdded.load() != JqState.nNumFinished.load())
	{
		JqExecuteOne(nullptr, 0);
	}
	return;
}


void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	if(JqIsDone(nJob))
	{
		return;
	}
	while(!JqIsDoneExt(nJob, nWaitFlag))
	{
		JqBlockWhileFrozen();
		bool bExecuted = false;

		if((nWaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			bExecuted = JqExecuteOneChild(nJob);
			if(!bExecuted)
			{
				bExecuted = JqExecuteOne();
			}
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			bExecuted = JqExecuteOneChild(nJob);
		}
		else if(0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			bExecuted = JqExecuteOne();
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
			}
		}
	}
}


void JqExecuteChildren(uint64_t nJob)
{
	JqExecuteOneChild(nJob);
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
	uint64_t nHandle = JqNextHandle();
	uint16_t nIndex = nHandle % JQ_TREE_BUFFER_SIZE2;
	JqJob2* pEntry = &JqState.m_Jobs2[nIndex];
	pEntry->PipeHandle.store(JqPipeHandleInvalid(), std::memory_order_release); // tag job as unfinishable

	JqJobFinish F = pEntry->Handle.load();

	JqJobFinish FOld = F;
	F.nStarted = nHandle;
	JQ_ASSERT(JQ_LE_WRAP(pEntry->Handle.load().nFinished, nHandle));
	bool bSuccess = pEntry->Handle.compare_exchange_weak(FOld, F);
	JQ_ASSERT(bSuccess);
	uint64_t nParentHandle = JqSelf();

	if(nParentHandle)
	{
		JqAttachChild(nParentHandle, nHandle);
	}
	else
	{
		pEntry->nRoot = nHandle;
	}
	JQ_CLEAR_FUNCTION(pEntry->Function);
	JqSelfPush(nHandle, 0);
	return nHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);

	uint16_t nIndex = nJob % JQ_TREE_BUFFER_SIZE2;
	uint64_t nHandle = JqState.m_Jobs2[nIndex].Handle.load().nStarted;
	JqState.m_Jobs2[nIndex].PipeHandle.store(JqPipeHandleNull(), std::memory_order_release);
	JqCheckFinished(nHandle);
}

uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1].nHandle : 0;
}

#endif
