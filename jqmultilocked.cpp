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

#define JQ_IMPL
#include "jq.h"
#include "jqinternal.h"


#define JQ_NUM_JOBS (JQ_PIPE_BUFFER_SIZE-1)
#define JQ_PRIORITY_ARRAY_SIZE (JQ_PRIORITY_SIZE)
#define JQ_PIPE_BITS 4


struct JqMutexLock;
struct JqPipe;

void 		JqStart(int nNumWorkers, uint32_t nJqInitFlags);
void 		JqCheckFinished(uint64_t nJob);
uint16_t 	JqIncrementStarted(JqPipe& Pipe, uint64_t nJob);
void 		JqIncrementFinished(JqPipe& Pipe, uint64_t nJob);
bool 		JqIncrementStartedLockless(JqPipe& Pipe, uint64_t nJob, uint16_t* pSubIndex);
bool 		JqIncrementFinishedLockless(JqPipe& Pipe, uint64_t nJob);
void		JqAttachChild(JqPipe& Pipe, uint64_t nParentJob, uint64_t nChildJob);
uint64_t 	JqDetachChild(JqPipe& Pipe, uint64_t nChildJob);
void 		JqExecuteJob(uint64_t nJob, uint16_t nJobSubIndex);
uint16_t 	JqTakeJob(JqPipe** pPipe, uint16_t* pJobSubIndex, uint32_t nNumPrio, uint8_t* pPrio);
uint16_t 	JqTakeChildJob(JqPipe& Pipe, uint64_t nJob, uint16_t* pJobSubIndex);
void 		JqWorker(int nThreadId);
uint64_t 	JqNextHandle(uint64_t nJob);
void 		JqWaitAll();
bool 		JqPriorityListAdd(JqPipe& Pipe, uint16_t nJobIndex);
void 		JqPriorityListRemove(JqPipe& Pipe, uint16_t nJobIndex);
uint64_t 	JqSelf();
uint32_t	JqSelfJobIndex();
int 		JqGetNumWorkers();
bool 		JqPendingJobs(JqPipe& Pipe, uint64_t nJob);
void 		JqSelfPush(uint64_t nJob, uint32_t nJobIndex);
void 		JqSelfPop(uint64_t nJob);
uint64_t 	JqFindHandle(JqPipe& Pipe);
bool		JqExecuteOne();
JqPipe& JqGetPipe(uint64_t nJob);

//todo:less locking
//todo:less signalling

struct JqSelfStack
{
	uint64_t nJob;
	uint32_t nJobIndex;
};

JQ_THREAD_LOCAL JqSelfStack JqSelfStack[JQ_MAX_JOB_STACK] = {{0}};
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;
JQ_THREAD_LOCAL uint32_t JqHasLock = 0;


struct JqJob
{
	JqFunction Function;

	uint64_t nStartedHandle;
	uint64_t nFinishedHandle;

	int32_t nRange;
	uint32_t nJobFlags;

	std::atomic<uint32_t> nNumJobs;
	std::atomic<uint32_t> nNumStarted;
	std::atomic<uint32_t> nNumFinished;

	//parent/child tree
	uint16_t nParent;
	uint16_t nFirstChild;
	uint16_t nSibling;

	//priority linked list
	uint16_t nLinkNext;
	uint16_t nLinkPrev;
	uint8_t nPrio;
	uint8_t nWaiters;
	uint8_t nWaitersWas;
	// int8_t nWaitIndex;

	// uint32_t nSignalCount;//debug

#ifdef JQ_ASSERT_SANITY
	int nTag;
#endif
};

#ifndef _WIN32
#include <pthread.h>
#endif
#include <atomic>
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable:4324)
#endif



#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS
//note: This is just arbitrary: given a suffiently random priority setup you might need to bump this.
struct JqJobHandle
{
	union
	{
		uint64_t nRawHandle;
		struct
		{
			uint64_t nHandle : 64 - JQ_PIPE_BITS;
			uint64_t nPipe : JQ_PIPE_BITS;
		};
	};
};
static_assert(sizeof(JqJobHandle) == 8, "sizeof(JqJobHandle) must be 8");
struct JqPriorityList
{
	uint16_t nHead;
	uint16_t nTail;
};
struct JQ_ALIGN_CACHELINE JqPipe
{
	JqJob Jobs[JQ_PIPE_BUFFER_SIZE];
	uint32_t nFreeJobs;
	std::atomic<JqPriorityList> PrioList;
	uint64_t nNextHandle;
	uint64_t nPipeId;
	JqMutex Mutex;
	JqConditionVariable WaitCond;

};

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore[JQ_MAX_SEMAPHORES];

	uint64_t m_SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t m_PipeNumSemaphores[JQ_NUM_PIPES];
	uint8_t m_PipeToSemaphore[JQ_NUM_PIPES][JQ_MAX_SEMAPHORES];
	uint8_t m_SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t m_SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int		m_ActiveSemaphores;
	// uint32_t m_JqInitFlags;
	JqAttributes m_Attributes;

	uint8_t m_nNumPipes[JQ_MAX_THREADS];
	uint8_t m_PipeList[JQ_MAX_THREADS][JQ_NUM_PIPES];
	uint8_t m_SemaphoreIndex[JQ_MAX_THREADS];


	JQ_THREAD WorkerThreads[JQ_MAX_THREADS];



	JqPipe m_JobPipes[JQ_NUM_PIPES];

	JqJobStackList m_StackSmall;
	JqJobStackList m_StackLarge;


	int nNumWorkers;
	int nStop;
	int nTotalWaiting;


	JqState_t()
		:nNumWorkers(0)
	{
	}


	JqStats Stats;
} JqState;

#ifdef _WIN32
#pragma warning(pop)
#endif


JQ_THREAD_LOCAL int JqSpinloop = 0; //prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumPipes = 0;
JQ_THREAD_LOCAL uint8_t g_JqPipes[JQ_NUM_PIPES] = { 0 };
JQ_THREAD_LOCAL JqJobStack* g_pJqJobStacks = 0;



void JqStart(JqAttributes* pAttr)
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

	JQ_ASSERT_NOT_LOCKED();

	JQ_ASSERT(JqState.nNumWorkers == 0);


	memset(JqState.m_PipeList, 0xff, sizeof(JqState.m_PipeList));
	memset(JqState.m_nNumPipes, 0, sizeof(JqState.m_nNumPipes));
	memset(JqState.m_SemaphoreMask, 0, sizeof(JqState.m_SemaphoreMask));
	memset(JqState.m_PipeNumSemaphores, 0, sizeof(JqState.m_PipeNumSemaphores));
	memset(JqState.m_PipeToSemaphore, 0, sizeof(JqState.m_PipeToSemaphore));
	memset(JqState.m_SemaphoreClients, 0, sizeof(JqState.m_SemaphoreClients));
	memset(JqState.m_SemaphoreClientCount, 0, sizeof(JqState.m_SemaphoreClientCount));
	JqState.m_ActiveSemaphores = 0;
	JqState.m_Attributes = *pAttr;
	JqState.nNumWorkers = pAttr->nNumWorkers;

	for (int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JqThreadConfig& C = JqState.m_Attributes.ThreadConfig[i];
		uint8_t nNumActivePipes = 0;
		uint64_t PipeMask = 0;
		static_assert(JQ_NUM_PIPES < 64, "wont fit in 64bit mask");
		for (uint32_t j = 0; j < C.nNumPipes; ++j)
		{
			if (C.nPipes[j] != 0xff)
			{
				JqState.m_PipeList[i][nNumActivePipes++] = C.nPipes[j];
				JQ_ASSERT(C.nPipes[j] < JQ_NUM_PIPES);
				PipeMask |= 1llu << C.nPipes[j];
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.m_nNumPipes[i] = nNumActivePipes;
		int nSelectedSemaphore = -1;
		for (int j = 0; j < JqState.m_ActiveSemaphores; ++j)
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
					JqState.m_PipeToSemaphore[j][JqState.m_PipeNumSemaphores[j]++] = (uint8_t)nSelectedSemaphore;
				}
			}
		}
		JQ_ASSERT(JqState.m_SemaphoreClientCount[nSelectedSemaphore] < JQ_MAX_SEMAPHORES);
		JqState.m_SemaphoreClients[nSelectedSemaphore][JqState.m_SemaphoreClientCount[nSelectedSemaphore]++] = (uint8_t)i;
		JqState.m_SemaphoreIndex[i] = (uint8_t)nSelectedSemaphore;
	}

	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		JqState.Semaphore[i].Init(JqState.m_SemaphoreClientCount[i] ? JqState.m_SemaphoreClientCount[i] : 1);
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
	JqState.nStop = 0;
	JqState.nTotalWaiting = 0;
	for(uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
	{
		JqPipe& Pipe = JqState.m_JobPipes[j];
		JqPriorityList PL = {0,0};
		for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
		{
			Pipe.Jobs[i].nStartedHandle = 0;
			Pipe.Jobs[i].nFinishedHandle = 0;	
			Pipe.PrioList.store(PL);
		}
		Pipe.nFreeJobs = JQ_NUM_JOBS;
		Pipe.nNextHandle = 1;
		Pipe.nPipeId = ((uint64_t)j) <<(64llu-JQ_PIPE_BITS);
		JqJobHandle H;
		H.nRawHandle = Pipe.nPipeId;

		JqJobHandle H1 = H;
		H1.nPipe += 1;
		//H1.nRawHandle = Pipe.nPipeId<<1;
		JQ_ASSERT(H.nPipe == j);
	}
	JqState.Stats.nNumFinished = 0;
	JqState.Stats.nNumLocks = 0;
	JqState.Stats.nNumSema = 0;
	JqState.Stats.nNumLocklessPops = 0;

	for(int i = 0; i < JqState.nNumWorkers; ++i)
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
	for (int i = 0; i < JqState.m_ActiveSemaphores; ++i)
	{
		JqState.Semaphore[i].Signal(JqState.nNumWorkers);
	}
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JQ_THREAD_JOIN(&JqState.WorkerThreads[i]);
		JQ_THREAD_DESTROY(&JqState.WorkerThreads[i]);
	}
	JqFreeAllStacks(JqState.m_StackSmall);
	JqFreeAllStacks(JqState.m_StackLarge);


	JqState.nNumWorkers = 0;
}

void JqSetThreadPipeConfig(JqThreadConfig* pConfig)
{
	JQ_ASSERT(g_nJqNumPipes == 0); // its not supported to change this value, nor is it supported to set it on worker threads. set on init instead.
	uint32_t nNumActivePipes = 0;
	JQ_ASSERT(pConfig->nNumPipes <= JQ_NUM_PIPES);
	for (uint32_t j = 0; j < pConfig->nNumPipes; ++j)
	{
		if (pConfig->nPipes[j] != 0xff)
		{
			g_JqPipes[nNumActivePipes++] = pConfig->nPipes[j];
			JQ_ASSERT(pConfig->nPipes[j] < JQ_NUM_PIPES);
		}
	}
	g_nJqNumPipes = nNumActivePipes;
}

void JqConsumeStats(JqStats* pStats)
{
	*pStats = JqState.Stats;
	pStats->nNumLocks = g_JqLockOps.exchange(0);
	pStats->nNumSema = g_JqSemaSignal.exchange(0) + g_JqCondSignal.exchange(0) + g_JqSemaWait.exchange(0) + g_JqCondWait.exchange(0);
	pStats->nNumLocklessPops = g_JqLocklessPops.exchange(0);
	JqState.Stats.nNumAdded = 0;
	JqState.Stats.nNumFinished = 0;
	JqState.Stats.nNumAddedSub = 0;
	JqState.Stats.nNumFinishedSub = 0;
	JqState.Stats.nNumCancelled = 0;
	JqState.Stats.nNumCancelledSub = 0;
	JqState.Stats.nNumLocks = 0;
	JqState.Stats.nNumSema = 0;
	JqState.Stats.nNumLocklessPops = 0;
}


void JqCheckFinished(uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint32_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE; 
	JqPipe& Pipe = JqGetPipe(nJob);
	JQ_ASSERT(JQ_LE_WRAP_SHIFT(Pipe.Jobs[nIndex].nFinishedHandle, nJob, JQ_PIPE_BITS));
	JQ_ASSERT(nJob == Pipe.Jobs[nIndex].nStartedHandle);
	if(0 == Pipe.Jobs[nIndex].nFirstChild && Pipe.Jobs[nIndex].nNumFinished == Pipe.Jobs[nIndex].nNumJobs)
	{
		uint16_t nParent = Pipe.Jobs[nIndex].nParent;
		if(nParent)
		{
			uint64_t nParentHandle = JqDetachChild(Pipe, nJob);
			Pipe.Jobs[nIndex].nParent = 0;
			JqCheckFinished(nParentHandle);
		}
		Pipe.Jobs[nIndex].nFinishedHandle = Pipe.Jobs[nIndex].nStartedHandle;
		JQ_CLEAR_FUNCTION(Pipe.Jobs[nIndex].Function);

		JqState.Stats.nNumFinished++;
		//kick waiting threads.
		int8_t nWaiters = Pipe.Jobs[nIndex].nWaiters;
		if(nWaiters != 0)
		{
			JqState.Stats.nNumSema++;
			Pipe.WaitCond.NotifyAll();
			Pipe.Jobs[nIndex].nWaiters = 0;
			Pipe.Jobs[nIndex].nWaitersWas = nWaiters;
		}
		else
		{
			Pipe.Jobs[nIndex].nWaitersWas = 0xff;
		}
		Pipe.nFreeJobs++;
	}
}

bool JqIncrementStartedLockless(JqPipe& Pipe, uint64_t nJob, uint16_t* pSubIndex)
{
	uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE; 
	JqJob* pJob = &Pipe.Jobs[nIndex];

	// JQ_ASSERT(Pipe.Jobs[nIndex].nNumJobs > Pipe.Jobs[nIndex].nNumStarted);
	uint32_t nSubIndex = 0;
	uint32_t nNumJobs = pJob->nNumJobs.load();
	do
	{
		nSubIndex = pJob->nNumStarted.load();
		if(nSubIndex == 0 || nSubIndex >= nNumJobs-1)
		{
			return false; //first & last cant be removed lockless
		}
	}while(!pJob->nNumStarted.compare_exchange_weak(nSubIndex, nSubIndex + 1));
	*pSubIndex = (uint16_t)nSubIndex;
	JQLSC(g_JqLocklessPops++);
	return true;
}

bool JqIncrementFinishedLockless(JqPipe& Pipe, uint64_t nJob)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);

	uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE; 
	JqJob* pJob = &Pipe.Jobs[nIndex];
	uint32_t nNumFinished = pJob->nNumFinished.load();
	uint32_t nNumJobs = pJob->nNumJobs.load();
	do
	{
		JQ_ASSERT(nNumFinished != nNumJobs);
		if(pJob->nStartedHandle != nJob || nNumFinished+1 == nNumJobs)
			return false;

	}while(!pJob->nNumFinished.compare_exchange_weak(nNumFinished, nNumFinished+1));
	JqState.Stats.nNumFinishedSub++;
	JQLSC(g_JqLocklessPops++);
	return true;
}

uint16_t JqIncrementStarted(JqPipe& Pipe, uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE; 
	JqJob* pJob = &Pipe.Jobs[nIndex];
	JQ_ASSERT(pJob->nNumJobs.load() > pJob->nNumStarted.load());
	uint16_t nSubIndex = (uint16_t)pJob->nNumStarted.fetch_add(1);
	if(pJob->nNumStarted == pJob->nNumJobs)
	{
		JqPriorityListRemove(Pipe, nIndex);
	}
	return nSubIndex;
}
void JqIncrementFinished(JqPipe& Pipe, uint64_t nJob)
{
	JQ_ASSERT_LOCKED();
	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
	uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE; 
	Pipe.Jobs[nIndex].nNumFinished++;
	JqState.Stats.nNumFinishedSub++;
	JqCheckFinished(nJob);
}
void JqIncrementFinishedHelper(JqPipe& Pipe, uint64_t nJob)
{
	if(!JqIncrementFinishedLockless(Pipe, nJob))
	{
		JqMutexLock L(Pipe.Mutex);
		JqIncrementFinished(Pipe, nJob);
	}
}

void JqAttachChild(JqPipe& Pipe, uint64_t nParentJob, uint64_t nChildJob)
{
	JQ_ASSERT_LOCKED();
	uint16_t nParentIndex = nParentJob % JQ_PIPE_BUFFER_SIZE;
	uint16_t nChildIndex = nChildJob % JQ_PIPE_BUFFER_SIZE;
	uint16_t nFirstChild = Pipe.Jobs[nParentIndex].nFirstChild;
	Pipe.Jobs[nChildIndex].nParent = nParentIndex;
	Pipe.Jobs[nChildIndex].nSibling = nFirstChild;
	Pipe.Jobs[nParentIndex].nFirstChild = nChildIndex;

	JQ_ASSERT(Pipe.Jobs[nParentIndex].nFinishedHandle != nParentJob);
	JQ_ASSERT(Pipe.Jobs[nParentIndex].nStartedHandle == nParentJob);
}

uint64_t JqDetachChild(JqPipe& Pipe, uint64_t nChildJob)
{
	uint16_t nChildIndex = nChildJob % JQ_PIPE_BUFFER_SIZE;
	uint16_t nParentIndex = Pipe.Jobs[nChildIndex].nParent;
	JQ_ASSERT(Pipe.Jobs[nChildIndex].nNumFinished == Pipe.Jobs[nChildIndex].nNumJobs);
	JQ_ASSERT(Pipe.Jobs[nChildIndex].nNumFinished == Pipe.Jobs[nChildIndex].nNumStarted);
	if(0 == nParentIndex)
	{
		return 0;
	}

	JQ_ASSERT(nParentIndex != 0);
	JQ_ASSERT(Pipe.Jobs[nChildIndex].nFirstChild == 0);
	uint16_t* pChildIndex = &Pipe.Jobs[nParentIndex].nFirstChild;
	JQ_ASSERT(Pipe.Jobs[nParentIndex].nFirstChild != 0); 
	while(*pChildIndex != nChildIndex)
	{
		JQ_ASSERT(Pipe.Jobs[*pChildIndex].nSibling != 0);
		pChildIndex = &Pipe.Jobs[*pChildIndex].nSibling;

	}
	JQ_ASSERT(pChildIndex);
	*pChildIndex = Pipe.Jobs[nChildIndex].nSibling;
	return Pipe.Jobs[nParentIndex].nStartedHandle;
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

void JqSelfPush(uint64_t nJob, uint32_t nSubIndex)
{	
	JqSelfStack[JqSelfPos].nJob = nJob;
	JqSelfStack[JqSelfPos].nJobIndex = nSubIndex;
	JqSelfPos++;
}

void JqSelfPop(uint64_t nJob)
{
	JQ_ASSERT(JqSelfPos != 0);
	JqSelfPos--;
	JQ_ASSERT(JqSelfStack[JqSelfPos].nJob == nJob);	
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




void JqContextRun(JqTransfer T)
{
	JqJobStack * pJobData = (JqJobStack*)T.data;

	pJobData->pPipe->Jobs[pJobData->nExternalId].Function(pJobData->nBegin, pJobData->nEnd);
	jq_jump_fcontext(T.fctx, (void*)447);
	JQ_BREAK();
}

JqJobStackList& JqGetJobStackList(uint32_t nFlags)
{
	bool bSmall = 0 != (nFlags&JQ_JOBFLAG_SMALL_STACK);
	return bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
}

void JqRunInternal(JqPipe& Pipe, uint32_t nWorkIndex, int nBegin, int nEnd)
{
	if(JQ_INIT_USE_SEPERATE_STACK == (JqState.m_Attributes.Flags & JQ_INIT_USE_SEPERATE_STACK))
	{
		uint32_t nFlags = Pipe.Jobs[nWorkIndex].nJobFlags;
		bool bSmall = 0 != (nFlags&JQ_JOBFLAG_SMALL_STACK);
		uint32_t nStackSize = bSmall ? JqState.m_Attributes.nStackSizeSmall : JqState.m_Attributes.nStackSizeLarge;
		JqJobStack* pJobData = JqAllocStack(JqGetJobStackList(nFlags), nStackSize, nFlags);
		void* pVerify = g_pJqJobStacks;
		JQ_ASSERT(pJobData->pLink == nullptr);
		pJobData->pLink = g_pJqJobStacks;
		pJobData->nBegin = nBegin;
		pJobData->nEnd = nEnd;
		pJobData->nExternalId = nWorkIndex;
		pJobData->pPipe = &Pipe;
		g_pJqJobStacks = pJobData;
		pJobData->pContextJob = jq_make_fcontext( pJobData->StackTop(), pJobData->StackSize(), JqContextRun);
		JqTransfer T = jq_jump_fcontext(pJobData->pContextJob, (void*) pJobData);
		JQ_ASSERT(T.data == (void*)447);
		g_pJqJobStacks = pJobData->pLink;
		pJobData->pLink = nullptr;
		JQ_ASSERT(pVerify == g_pJqJobStacks);
		JQ_ASSERT(pJobData->GUARD[0] == 0xececececececececll);
		JQ_ASSERT(pJobData->GUARD[1] == 0xececececececececll);
		JqFreeStack(JqGetJobStackList(nFlags), pJobData);
	}
	else
	{
		Pipe.Jobs[nWorkIndex].Function(nBegin, nEnd);
	}
}






void JqExecuteJob(JqPipe& Pipe, uint64_t nJob, uint16_t nSubIndex)
{
	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JqSelfPush(nJob, nSubIndex);
	uint16_t nWorkIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	uint16_t nNumJobs = (uint16_t)Pipe.Jobs[nWorkIndex].nNumJobs;
	int nRange = Pipe.Jobs[nWorkIndex].nRange;
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;	
	int nStart = JqGetRangeStart(nSubIndex, nFraction, nRemainder);
	int nEnd = JqGetRangeStart(nSubIndex+1, nFraction, nRemainder);
	JqRunInternal(Pipe, nWorkIndex, nStart, nEnd);
	JqSelfPop(nJob);
}


uint16_t JqTakeJob(JqPipe** pPipe, uint16_t* pSubIndex, uint32_t nNumPrio, uint8_t* pPrio)
{
	if(nNumPrio)
	{
		for(uint32_t i = 0; i < nNumPrio; i++)
		{
			JqPipe& Pipe = JqState.m_JobPipes[pPrio[i]];
			JqPriorityList L = Pipe.PrioList.load(std::memory_order_acquire);
			uint16_t nIndex = L.nHead;
			if(nIndex)
			{
				if(JqIncrementStartedLockless(Pipe, Pipe.Jobs[nIndex].nStartedHandle, pSubIndex))
				{
					*pPipe = &Pipe;
					return nIndex;
				}
				//something there, so take lock
				{
					JqMutexLock LSCOPE(Pipe.Mutex);					
					L = Pipe.PrioList.load(std::memory_order_acquire);
					nIndex = L.nHead;
					if(nIndex)
					{
						*pSubIndex = JqIncrementStarted(Pipe, Pipe.Jobs[nIndex].nStartedHandle);
						*pPipe = &Pipe;
						return nIndex;
					}
				}
			}
		}
	}
	else
	{
		for(int i = 0; i < JQ_NUM_PIPES; ++i)
		{

			JqPipe& Pipe = JqState.m_JobPipes[i];
			JqPriorityList L = Pipe.PrioList.load(std::memory_order_acquire);
			uint16_t nIndex = L.nHead;
			if(nIndex)
			{
				if(JqIncrementStartedLockless(Pipe, Pipe.Jobs[nIndex].nStartedHandle, pSubIndex))
				{
					*pPipe = &Pipe;
					return nIndex;
				}

				JqMutexLock LSCOPE(Pipe.Mutex);
				L = Pipe.PrioList.load(std::memory_order_acquire);
				nIndex = L.nHead;
				if(nIndex)
				{
					*pSubIndex = JqIncrementStarted(Pipe, Pipe.Jobs[nIndex].nStartedHandle);
					*pPipe = &Pipe;
					return nIndex;
				}
			}
		}
	}
	return 0;
}
#ifdef JQ_ASSERT_SANITY
void JqTagChildren(JqPipe& Pipe, uint16_t nRoot)
{
	for(int i = 1; i < JQ_PIPE_BUFFER_SIZE; ++i)
	{
		JQ_ASSERT(Pipe.Jobs[i].nTag == 0);
		if(nRoot == i)
		{
			Pipe.Jobs[i].nTag = 1;
		}
		else
		{
			int nParent = Pipe.Jobs[i].nParent;
			while(nParent)
			{
				if(nParent == nRoot)
				{
					Pipe.Jobs[i].nTag = 1;
					break;
				}
				nParent = Pipe.Jobs[nParent].nParent;
			}
		}
	}

}
void JqCheckTagChildren(JqPipe& Pipe, uint16_t nRoot)
{
	for(int i = 1; i < JQ_PIPE_BUFFER_SIZE; ++i)
	{
		JQ_ASSERT(Pipe.Jobs[i].nTag == 0);
		bool bTagged = false;
		if(nRoot == i)
			bTagged = false;
		else
		{
			int nParent = Pipe.Jobs[i].nParent;
			while(nParent)
			{
				if(nParent == nRoot)
				{
					bTagged = true;
					break;
				}
				nParent = Pipe.Jobs[nParent].nParent;
			}
		}
		JQ_ASSERT(bTagged == (1==Pipe.Jobs[i].nTag));
	}
}

void JqLoopChildren(JqPipe& Pipe, uint16_t nRoot)
{
	int nNext = Pipe.Jobs[nRoot].nFirstChild;
	while(nNext != nRoot && nNext)
	{
		while(Pipe.Jobs[nNext].nFirstChild)
			nNext = Pipe.Jobs[nNext].nFirstChild;
		JQ_ASSERT(Pipe.Jobs[nNext].nTag == 1);
		Pipe.Jobs[nNext].nTag = 0;
		if(Pipe.Jobs[nNext].nSibling)
			nNext = Pipe.Jobs[nNext].nSibling;
		else
		{
			//search up
			nNext = Pipe.Jobs[nNext].nParent;
			while(nNext != nRoot)
			{
				JQ_ASSERT(Pipe.Jobs[nNext].nTag == 1);
				Pipe.Jobs[nNext].nTag = 0;
				if(Pipe.Jobs[nNext].nSibling)
				{
					nNext = Pipe.Jobs[nNext].nSibling;
					break;
				}
				else
				{
					nNext = Pipe.Jobs[nNext].nParent;
				}

			}
		}
	}

	JQ_ASSERT(Pipe.Jobs[nRoot].nTag == 1);
	Pipe.Jobs[nRoot].nTag = 0;
}
#endif


//Depth first. Once a node is visited all child nodes have been visited
uint16_t JqTreeIterate(JqPipe& Pipe, uint64_t nJob, uint16_t nCurrent)
{
	JQ_ASSERT(nJob);
	uint16_t nRoot = nJob % JQ_PIPE_BUFFER_SIZE;
	if(nRoot == nCurrent)
	{
		while(Pipe.Jobs[nCurrent].nFirstChild)
			nCurrent = Pipe.Jobs[nCurrent].nFirstChild;
	}
	else
	{
		//once here all child nodes _have_ been processed.
		if(Pipe.Jobs[nCurrent].nSibling)
		{
			nCurrent = Pipe.Jobs[nCurrent].nSibling;
			while(Pipe.Jobs[nCurrent].nFirstChild) //child nodes first.
				nCurrent = Pipe.Jobs[nCurrent].nFirstChild;
		}
		else
		{
			nCurrent = Pipe.Jobs[nCurrent].nParent;
		}
	}
	return nCurrent;
}


//this code is O(n) where n is the no. of child nodes.
//I wish this could be written in a simpler way
uint16_t JqTakeChildJob(JqPipe& Pipe, uint64_t nJob, uint16_t* pSubIndexOut)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeChildJob", 0xff);
	JqMutexLock L(Pipe.Mutex);
	JQ_ASSERT_LOCKED();
#if JQ_ASSERT_SANITY
	{
		//verify that the child iteration is sane
		for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
		{
			Pipe.Jobs[i].nTag = 0;
		}
		JqTagChildren(Pipe, nJob%JQ_PIPE_BUFFER_SIZE);
		uint16_t nIndex = nJob%JQ_PIPE_BUFFER_SIZE;
		uint16_t nRootIndex = nJob % JQ_PIPE_BUFFER_SIZE;
		do
		{
			nIndex = JqTreeIterate(Pipe, nJob, nIndex);
			JQ_ASSERT(Pipe.Jobs[nIndex].nTag == 1);
			Pipe.Jobs[nIndex].nTag = 0;
		}while(nIndex != nRootIndex);
		for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
		{
			JQ_ASSERT(Pipe.Jobs[i].nTag == 0);
		}
	}
#endif

	uint16_t nRoot = nJob % JQ_PIPE_BUFFER_SIZE;
	uint16_t nIndex = nRoot;

	do
	{
		nIndex = JqTreeIterate(Pipe, nJob, nIndex);
		JQ_ASSERT(nIndex);

		if(Pipe.Jobs[nIndex].nNumStarted < Pipe.Jobs[nIndex].nNumJobs)
		{
			*pSubIndexOut = JqIncrementStarted(Pipe, Pipe.Jobs[nIndex].nStartedHandle);
			return nIndex;
		}
	}while(nIndex != nRoot);

	return 0;
}

bool JqExecuteOne()
{
	return JqExecuteOne(g_JqPipes, (uint8_t)g_nJqNumPipes);
}

bool JqExecuteOne(uint8_t nPipe)
{
	return JqExecuteOne(&nPipe, 1);
}

bool JqExecuteOne(uint8_t* pPipes, uint8_t nNumPipes)
{
	uint16_t nSubIndex = 0;
	uint16_t nWork;
	JqPipe* pPipe = nullptr;
	{
		nWork = JqTakeJob(&pPipe, &nSubIndex, nNumPipes, pPipes);
	}
	if(!nWork)
		return false;
	JqExecuteJob(*pPipe, pPipe->Jobs[nWork].nStartedHandle, nSubIndex);
	JqIncrementFinishedHelper(*pPipe, pPipe->Jobs[nWork].nStartedHandle);
	return true;
}

void JqWorker(int nThreadId)
{

	uint8_t* pPipes = JqState.m_PipeList[nThreadId];
	uint32_t nNumPipes = JqState.m_nNumPipes[nThreadId];
	g_nJqNumPipes = nNumPipes; //even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqPipes, pPipes, nNumPipes);
	int nSemaphoreIndex = JqState.m_SemaphoreIndex[nThreadId];


#if MICROPROFILE_ENABLED
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker)-1, "JqWorker %d %08llx", nThreadId, JqState.m_SemaphoreMask[nSemaphoreIndex]);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.nStop)
	{
		JqPipe* pPipe = nullptr;;
		uint16_t nWork = 0;
		do
		{
			uint16_t nSubIndex = 0;

			nWork = JqTakeJob(&pPipe, &nSubIndex, nNumPipes, pPipes);


			if(nWork)
			{
				JqExecuteJob(*pPipe, pPipe->Jobs[nWork].nStartedHandle, nSubIndex);
				// JqIncrementFinishedHelper(
				// JqMutexLock L(pPipe->Mutex);
				JqIncrementFinishedHelper(*pPipe, pPipe->Jobs[nWork].nStartedHandle);
				pPipe = nullptr;
			}


		}while(nWork != 0);

		JqState.Semaphore[nSemaphoreIndex].Wait();
	}
#ifdef JQ_MICROPROFILE
	MicroProfileOnThreadExit();
#endif
}
uint64_t JqNextHandle(uint64_t nJob)
{
	nJob++;
	if(0 == (nJob%JQ_PIPE_BUFFER_SIZE))
	{
		nJob++;
	}
	return (nJob << JQ_PIPE_BITS)>>JQ_PIPE_BITS;
}
uint64_t JqFindHandle(JqPipe& Pipe)
{
	JQ_ASSERT_LOCKED();
	while(!Pipe.nFreeJobs)
	{
		if(JqSelfPos < JQ_MAX_JOB_STACK)
		{
			Pipe.Mutex.Unlock();
			JQ_MICROPROFILE_SCOPE("AddExecute", 0xff); // if this starts happening the job queue size should be increased..
			JqPipe* pJobPipe = 0;
			uint16_t nSubIndex = 0;			
			uint16_t nIndex = JqTakeJob(&pJobPipe, &nSubIndex, 0, nullptr);
			if(nIndex)
			{
				JqExecuteJob(*pJobPipe, pJobPipe->Jobs[nIndex].nStartedHandle, nSubIndex);
				JqIncrementFinishedHelper(*pJobPipe, pJobPipe->Jobs[nIndex].nStartedHandle);
			}
			Pipe.Mutex.Lock();
		}
		else
		{
			JQ_BREAK(); //out of job queue space. increase JQ_PIPE_BUFFER_SIZE or create fewer jobs
		}
	}
	JQ_ASSERT(Pipe.nFreeJobs != 0);
	uint64_t nNextHandle = Pipe.nNextHandle;
	uint16_t nCount = 0;
	while(JqPendingJobs(Pipe, nNextHandle))
	{
		nNextHandle = JqNextHandle(nNextHandle);
		JQ_ASSERT(nCount++ < JQ_PIPE_BUFFER_SIZE);
	}
	Pipe.nNextHandle = JqNextHandle(nNextHandle);

	Pipe.nFreeJobs--;	
	return nNextHandle | Pipe.nPipeId;
}

JQ_API void			JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nWaitFlag)
{
	uint64_t nJob = JqAdd(JobFunc, nPrio, nNumJobs, nRange);
	JqWait(nJob, nWaitFlag);
}

bool JqCancel(uint64_t nJob)
{
	JqPipe& Pipe = JqGetPipe(nJob);
	JqMutexLock Lock(Pipe.Mutex);
	uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;

	JqJob* pEntry = &Pipe.Jobs[nIndex];
	if(pEntry->nStartedHandle != nJob)
		return false;
	if(pEntry->nNumStarted != 0)
		return false;
	uint32_t nNumJobs = pEntry->nNumJobs;
	pEntry->nNumJobs = 0;

	JqPriorityListRemove(Pipe, nIndex);
	JqCheckFinished(nJob);
	JQ_ASSERT(JqIsDone(nJob));

	JqState.Stats.nNumCancelled++;
	JqState.Stats.nNumCancelledSub += nNumJobs;
	return true;
}




uint64_t JqAdd(JqFunction JobFunc, uint8_t nPipe, int nNumJobs, int nRange, uint32_t nJobFlags)
{
	uint64_t nParent = 0 != (nJobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf();
	JqJobHandle HParent;
	HParent.nRawHandle = nParent;
	nPipe = (uint8_t)(nParent == 0 ? nPipe : HParent.nPipe);
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

	bool bSignal = false;
	auto& Pipe = JqState.m_JobPipes[nPipe];
	uint64_t nNextHandle = 0;
	{
		JqMutexLock Lock(Pipe.Mutex);
		nNextHandle = JqFindHandle(Pipe);
		uint16_t nIndex = nNextHandle % JQ_PIPE_BUFFER_SIZE;

		JqJob* pEntry = &Pipe.Jobs[nIndex];
		JQ_ASSERT(JQ_LT_WRAP(pEntry->nFinishedHandle, nNextHandle));
		pEntry->nStartedHandle = nNextHandle;
		JQ_ASSERT(nNumJobs <= 0xffff);
		pEntry->nNumJobs = (uint16_t)nNumJobs;
		pEntry->nNumStarted = 0;
		pEntry->nNumFinished = 0;
		pEntry->nRange = nRange;
		pEntry->nJobFlags = nJobFlags;

		uint64_t nParentHandle = nParent;
		pEntry->nParent = nParentHandle % JQ_PIPE_BUFFER_SIZE;
		if(pEntry->nParent)
		{
			JqAttachChild(Pipe, nParentHandle, nNextHandle);
		}
		pEntry->Function = JobFunc;
		pEntry->nWaiters = 0;
		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += nNumJobs;
		bSignal = JqPriorityListAdd(Pipe, nIndex);
	}

	if(bSignal)
	{
		uint32_t nNumSema = JqState.m_PipeNumSemaphores[nPipe];
		for (uint32_t i = 0; i < nNumSema; ++i)
		{
			int nSemaIndex = JqState.m_PipeToSemaphore[nPipe][i];
			JqState.Semaphore[nSemaIndex].Signal(nNumJobs);
		}
	}

	//JqState.SemaphoreAll.Signal(nNumJobs);
	//if(eJobType == EJQ_TYPE_SHORT)
	//{
	//	JqState.SemaphoreSmall.Signal(nNumJobs);
	//}
	return nNextHandle;
}

void JqDump()
{

}

JqPipe& JqGetPipe(uint64_t nJob)
{
	JqJobHandle H;
	H.nRawHandle = nJob;
	return JqState.m_JobPipes[H.nPipe];

}

bool JqIsDone(uint64_t nJob)
{
	JqPipe& Pipe = JqGetPipe(nJob);
	uint64_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	uint64_t nFinishedHandle = Pipe.Jobs[nIndex].nFinishedHandle;
	uint64_t nStartedHandle = Pipe.Jobs[nIndex].nStartedHandle;
	JQ_ASSERT(JQ_LE_WRAP_SHIFT(nFinishedHandle, nStartedHandle, JQ_PIPE_BITS));
	int64_t nDiff = (int64_t)(nFinishedHandle - nJob);
	JQ_ASSERT((nDiff >= 0) == JQ_LE_WRAP_SHIFT(nJob, nFinishedHandle, JQ_PIPE_BITS));
	return JQ_LE_WRAP_SHIFT(nJob, nFinishedHandle, JQ_PIPE_BITS);
}

bool JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlags)
{
	bool bIsDone = JqIsDone(nJob);
	if(!bIsDone && 0 != (nWaitFlags & JQ_WAITFLAG_IGNORE_CHILDREN))
	{
		JqPipe& Pipe = JqGetPipe(nJob);
		uint64_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
		return Pipe.Jobs[nIndex].nNumJobs == Pipe.Jobs[nIndex].nNumFinished;
	}
	return bIsDone;
}


bool JqPendingJobs(JqPipe& Pipe, uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	JQ_ASSERT(JQ_LE_WRAP(Pipe.Jobs[nIndex].nFinishedHandle, Pipe.Jobs[nIndex].nStartedHandle));
	return Pipe.Jobs[nIndex].nFinishedHandle != Pipe.Jobs[nIndex].nStartedHandle;
}


void JqWaitAll()
{
	while(1)
	{
		JqPipe* pPipe = nullptr;
		{
			uint16_t nWork = 0;
			uint16_t nSubIndex = 0;
			nWork = JqTakeJob(&pPipe, &nSubIndex, 0, 0);
			if(nWork)
			{
				JqExecuteJob(*pPipe, pPipe->Jobs[nWork].nStartedHandle, nSubIndex);
				// JqMutexLock L(pPipe->Mutex);
				JqIncrementFinishedHelper(*pPipe, pPipe->Jobs[nWork].nStartedHandle);
				pPipe = nullptr;
				continue;
			}
		}
		JQ_USLEEP(1000);
		bool bAllDone = true;
		for(uint32_t i = 0; i < JQ_NUM_PIPES; ++i)
		{
			if(JqState.m_JobPipes[i].nFreeJobs != JQ_NUM_JOBS)
			{
				bAllDone = false;
			}
		}
		if(bAllDone)
		{
			break;
		}
	}
}


void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	uint16_t nIndex = 0;
	JqPipe* pPipe = nullptr;
	if(JqIsDone(nJob))
	{
		return;
	}

	JqPipe& Pipe = JqGetPipe(nJob);

	while(!JqIsDoneExt(nJob, nWaitFlag))
	{

		uint16_t nSubIndex = 0;
		if((nWaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			nIndex = JqTakeChildJob(Pipe, nJob, &nSubIndex);
			if(nIndex)
			{
				pPipe = &Pipe;
			}
			else
			{
				nIndex = JqTakeJob(&pPipe, &nSubIndex, g_nJqNumPipes, g_JqPipes);
				if(!nIndex)
					pPipe = nullptr;

			}

			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			// if(JqIsDoneExt(nJob, nWaitFlag)) 
			// 	return;
			// nIndex = JqTakeChildJob(nJob, &nSubIndex);
			// if(!nIndex)
			// {
			// 	nIndex = JqTakeJob(&nSubIndex, g_nJqNumPipes, g_JqPipes);
			// }
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			nIndex = JqTakeChildJob(Pipe, nJob, &nSubIndex);
			if(nIndex)
			{
				pPipe = &Pipe;
			}


			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			// if(JqIsDoneExt(nJob, nWaitFlag)) 
			// 	return;
			// nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		else if(0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			nIndex = JqTakeJob(&pPipe, &nSubIndex, g_nJqNumPipes, g_JqPipes);
			if(!nIndex)
				pPipe = nullptr;
		}
		else
		{
			JQ_BREAK();
		}
		if(!nIndex)
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
					JqSpinloop |= result; //write it somewhere so the optimizer can't remote it
				}while( (1000000ull*(JqTick()-nTick)) / nTicksPerSecond < nUsWaitTime);


			}
			else if(nWaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(nUsWaitTime);
			}
			else
			{
				if(JqIsDoneExt(nJob, nWaitFlag))
				{
					return;
				}
				JqMutexLock lock(Pipe.Mutex);
				uint16_t nJobIndex = nJob % JQ_PIPE_BUFFER_SIZE;
				Pipe.Jobs[nJobIndex].nWaiters++;
				JqState.Stats.nNumWaitCond++;
				Pipe.WaitCond.Wait(Pipe.Mutex);
				Pipe.Jobs[nJobIndex].nWaiters--;
			}
		}
		else
		{
			JqExecuteJob(*pPipe, pPipe->Jobs[nIndex].nStartedHandle, nSubIndex);
			// JqMutexLock L(pPipe->Mutex);
			JqIncrementFinishedHelper(*pPipe, pPipe->Jobs[nIndex].nStartedHandle);
		}
	}

}


void JqExecuteChildren(uint64_t nJob)
{
	if(JqIsDone(nJob))
	{
		return;
	}
	JqPipe& Pipe = JqGetPipe(nJob);
	uint16_t nIndex = 0;
	do 
	{		
		uint16_t nSubIndex = 0;
		{
			JqMutexLock lock(Pipe.Mutex);
			if(nIndex)
				JqIncrementFinished(Pipe, Pipe.Jobs[nIndex].nStartedHandle);

			if(JqIsDone(nJob)) 
				return;

			nIndex = JqTakeChildJob(Pipe, nJob, &nSubIndex);
		}
		if(nIndex)
		{
			JqExecuteJob(Pipe, Pipe.Jobs[nIndex].nStartedHandle, nSubIndex);
		}

	}while(nIndex);
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


uint64_t JqGroupBegin(uint8_t nPipe)
{
	JQ_ASSERT(nPipe < JQ_NUM_PIPES);
	JqPipe& Pipe = JqState.m_JobPipes[nPipe];
	JqMutexLock Lock(Pipe.Mutex);
	uint64_t nNextHandle = JqFindHandle(Pipe);
	uint16_t nIndex = nNextHandle % JQ_PIPE_BUFFER_SIZE;
	JqJob* pEntry = &Pipe.Jobs[nIndex];
	JQ_ASSERT(JQ_LE_WRAP(pEntry->nFinishedHandle, nNextHandle));
	pEntry->nStartedHandle = nNextHandle;
	pEntry->nNumJobs = 1;
	pEntry->nNumStarted = 1;
	pEntry->nNumFinished = 0;
	pEntry->nRange = 0;
	uint64_t nParentHandle = JqSelf();
	pEntry->nParent = nParentHandle % JQ_PIPE_BUFFER_SIZE;
	if(pEntry->nParent)
	{
		JqAttachChild(Pipe, nParentHandle, nNextHandle);
	}
	JQ_CLEAR_FUNCTION(pEntry->Function);
	pEntry->nPrio = 7;
	pEntry->nWaiters = 0;
	JqSelfPush(nNextHandle, 0);
	return nNextHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);
	JqPipe& Pipe = JqGetPipe(nJob);

	JqMutexLock lock(Pipe.Mutex);
	JqIncrementFinished(Pipe, nJob);
}



uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1].nJob : 0;
}


bool JqPriorityListAdd(JqPipe& Pipe, uint16_t nIndex)
{
	JQ_ASSERT_LOCKED();
	JqPriorityList L = Pipe.PrioList.load();
	JQ_ASSERT(Pipe.Jobs[nIndex].nLinkNext == 0);
	JQ_ASSERT(Pipe.Jobs[nIndex].nLinkPrev == 0);
	// uint16_t nTail = Pipe.nPrioListTail;
	if(L.nTail != 0)
	{
		JQ_ASSERT(Pipe.Jobs[L.nTail].nLinkNext == 0);
		Pipe.Jobs[L.nTail].nLinkNext = nIndex;
		Pipe.Jobs[nIndex].nLinkPrev = L.nTail;
		Pipe.Jobs[nIndex].nLinkNext = 0;
		L.nTail = nIndex;
	}
	else
	{
		JQ_ASSERT(L.nHead == 0);
		L.nHead = nIndex;
		L.nTail = nIndex;
		Pipe.Jobs[nIndex].nLinkNext = 0;
		Pipe.Jobs[nIndex].nLinkPrev = 0;
	}
	Pipe.PrioList.store(L, std::memory_order_release);
	return true;
}
void JqPriorityListRemove(JqPipe& Pipe, uint16_t nIndex)
{
	JQ_ASSERT_LOCKED();
	// uint8_t nPrio = Pipe.Jobs[nIndex].nPrio;
	JqPriorityList L = Pipe.PrioList.load();
	uint16_t nNext = Pipe.Jobs[nIndex].nLinkNext;
	uint16_t nPrev = Pipe.Jobs[nIndex].nLinkPrev;
	Pipe.Jobs[nIndex].nLinkNext = 0;
	Pipe.Jobs[nIndex].nLinkPrev = 0;

	if(nNext != 0)
	{
		Pipe.Jobs[nNext].nLinkPrev = nPrev;
	}
	else
	{
		JQ_ASSERT(L.nTail == nIndex);
		L.nTail = nPrev;
	}
	if(nPrev != 0)
	{
		Pipe.Jobs[nPrev].nLinkNext = nNext;
	}
	else
	{
		JQ_ASSERT(L.nHead == nIndex);
		L.nHead = nNext;
	}
	Pipe.PrioList.store(L, std::memory_order_release);

}

