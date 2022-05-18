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
//
//
// Flow
//
// Add:
//	* Allocate header (inc counter)
//	* Update header as reserved
//	* Fill in header
//  * if there is a parent, increment its pending count by no. of jobs
//	* lock pipe mtx
//  	* add to pipe
// 	* unlock
//
// Take job
// 	* lock pipe mtx
//		* decrement front jobs pending_start count
//		* if 0 flip remove from pending
//  * unlock mutex
//
//
// Finish:
//	* Decrement pending count
//	* if 0, tag as finished
//	* decrement parents pending count
//	* lock pipe mtx
//
// Reserve
//	* Allocate a header, set its preq count to 1
//
// Release Reseverd  (can be used to do barriers)
// 	* decrement preq count by 1
//
//
// Add Dependency
// 	* increment preq count on a reserved header
//	* Cannot be done on a started job(IE must use reserve)
//
// Execute Children
//
//

#define JQ_IMPL
#include "jq.h"
#include "jqinternal.h"

#define JQ_NUM_JOBS (JQ_PIPE_BUFFER_SIZE - 1)

#define JQ_JOBFLAG_UNINITIALIZED 0x80

struct JqMutexLock;

// void	 JqAttachChild(uint64_t nParentJob, uint64_t nChildJob);
// uint64_t JqDetachChild(uint64_t nChildJob);
void	 JqWorker(int nThreadId);
void	 JqQueuePush(uint8_t nQueue, uint16_t nJobIndex);
uint16_t JqQueuePop(uint8_t Queue, uint16_t* OutSubJob) void JqQueueRemove(uint8_t nQueue, uint16_t nJobIndex);
bool	 JqPendingJobs(uint64_t nJob);
void	 JqSelfPush(uint64_t nJob, uint32_t nJobIndex);
void	 JqSelfPop(uint64_t nJob);

void JqDecPrecondtion(uint64_t Handle, int Count);
void JqIncPrecondtion(uint64_t Handle, int Count);

JqMutex& JqGetQueueMutex(uint16_t QueueIndex);
JqMutex& JqGetJobMutex(uint16_t JobIndex);

struct JqSelfStack
{
	uint64_t nJob;
	uint32_t nJobIndex;
};

JQ_THREAD_LOCAL JqSelfStack JqSelfStack[JQ_MAX_JOB_STACK] = { { 0 } };
JQ_THREAD_LOCAL uint32_t	JqSelfPos					  = 0;
JQ_THREAD_LOCAL uint32_t	JqHasLock					  = 0;

// union JqJobLinkHelper
// {
// 	struct
// 	{
// 		uint16_t nNext;		 // next / head index
// 		uint16_t nPrev;		 // prev / tail index
// 		uint16_t nRemaining; // no of jobs started
// 		uint16_t nVersion;	 // rolling counter to prevent accidental atomics.
// 	};
// 	uint64_t nValue;
// };
// typedef std::atomic<uint64_t> JqJobLink;

// struct JQ_ALIGN_CACHELINE JqPipeList
// {
// 	JqJobLink Link;
// };

static_assert(sizeof(JqPipeList) >= JQ_CACHE_LINE_SIZE);
struct JqJob
{
	JqFunction Function;

	std::atomic<uint64_t> StartedHandle;  /// Handle which has been added to the queue
	std::atomic<uint64_t> FinishedHandle; /// Handle which was last finished
	std::atomic<uint64_t> ClaimedHandle;  /// Handle which has claimed this header

	std::atomic<uint64_t> PendingFinish;	 /// No. of jobs & (direct) child jobs that need to finish in order for this to be finished.
	std::atomic<uint64_t> PendingStart;		 /// No. of Jobs that needs to be Started
	std::atomic<uint64_t> PreconditionCount; /// No. of Preconditions that need to finish, before this can be enqueued.
	uint64_t			  Parent;			 /// Handle of parent
	bool				  Locked;			 /// Set when reserving a barrier

	uint16_t NumJobs;	 /// Num Jobs to launch
	uint64_t Range;		 /// Range to pass to jobs
	uint32_t JobFlags;	 /// Job Flags
	uint8_t	 Queue;		 /// Priority of the job
	uint8_t	 Waiters;	 /// Set when waiting
	uint8_t	 WaitersWas; /// Prev wait flag(debug only)

	uint16_t LinkNext;
	uint16_t LinkPrev;
	// uint64_t nStartedHandle;
	// uint64_t nFinishedHandle;

	// int32_t	 nRange;
	// uint32_t nJobFlags;

	// uint16_t nNumJobs;
	// // uint16_t nNumStarted;
	// uint16_t nNumFinished;

	// // parent/child tree
	// uint16_t nParent;
	// uint16_t nFirstChild;
	// uint16_t nSibling;

	// // priority linked list

	// // uint16_t nLinkNext;
	// // uint16_t nLinkPrev;

	// uint8_t nPrio;
	// uint8_t nWaiters;
	// uint8_t nWaitersWas;
	// // int8_t nWaitIndex;

	// uint64_t nPreconditionFirst;   // first job that has this job as a precondition.
	// uint64_t nPreconditionSibling; // In case this job is registered as a precodition, linked list from the job its waiting on.

	// uint8_t nStalled;

	// // uint32_t nSignalCount;//debug

	// // priority linked list
	// JqJobLink Link;

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
#pragma warning(disable : 4324)
#endif

#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS

#define JQ_NUM_JOB_MUTEX 32

struct JqQueue
{
	JqMutex	 Mutex;
	uint16_t ListHead;
	uint16_t ListTail;
};
// note: This is just arbitrary: given a suffiently random priority setup you might need to bump this.

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore[JQ_MAX_SEMAPHORES];
	// JqMutex				Mutex;
	JqConditionVariable WaitCond;

	uint64_t m_SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t	 m_PipeNumSemaphores[JQ_NUM_PIPES];
	uint8_t	 m_PipeToSemaphore[JQ_NUM_PIPES][JQ_MAX_SEMAPHORES];
	uint8_t	 m_SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t	 m_SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int		 m_ActiveSemaphores;
	// uint32_t m_JqInitFlags;
	JqAttributes m_Attributes;

	uint8_t		   m_nNumPipes[JQ_MAX_THREADS];
	uint8_t		   m_PipeList[JQ_MAX_THREADS][JQ_NUM_PIPES];
	uint8_t		   m_SemaphoreIndex[JQ_MAX_THREADS];
	JQ_THREAD	   WorkerThreads[JQ_MAX_THREADS];
	JqJobStackList m_StackSmall;
	JqJobStackList m_StackLarge;
	int			   nNumWorkers;
	int			   nStop;
	int			   nTotalWaiting;
	// uint32_t	   nFreeJobs;

	std::atomic<uint32_t> FreeJobs;
	std::atomic<uint64_t> NextHandle;

	JqPipeOrder ThreadConfig[JQ_MAX_THREADS];
	JqJob		Jobs[JQ_PIPE_BUFFER_SIZE];

	JqQueue Queues[JQ_NUM_PIPES];

	JqMutex MutexJob[JQ_NUM_JOB_MUTEX];

	// JqPipeList	   Pipes[JQ_NUM_PIPES];

	// uint16_t nPrioListHead[JQ_NUM_PIPES];
	// uint16_t nPrioListTail[JQ_NUM_PIPES];

	JqState_t()
		: nNumWorkers(0)
	{
	}

	JqStats Stats;
} JqState;

#ifdef _WIN32
#pragma warning(pop)
#endif

JQ_THREAD_LOCAL int		 JqSpinloop				  = 0; // prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumQueues			  = 0;
JQ_THREAD_LOCAL uint8_t	 g_JqQueues[JQ_NUM_PIPES] = { 0 };
JQ_THREAD_LOCAL JqJobStack* g_pJqJobStacks		  = 0;

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

	JQ_ASSERT_NOT_LOCKED(JqState.Mutex);

	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & (uint64_t)&JqState) == 0);
	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.nNumWorkers == 0);

	memset(JqState.m_PipeList, 0xff, sizeof(JqState.m_PipeList));
	memset(JqState.m_nNumPipes, 0, sizeof(JqState.m_nNumPipes));
	memset(JqState.m_SemaphoreMask, 0, sizeof(JqState.m_SemaphoreMask));
	memset(JqState.m_PipeNumSemaphores, 0, sizeof(JqState.m_PipeNumSemaphores));
	memset(JqState.m_PipeToSemaphore, 0, sizeof(JqState.m_PipeToSemaphore));
	memset(JqState.m_SemaphoreClients, 0, sizeof(JqState.m_SemaphoreClients));
	memset(JqState.m_SemaphoreClientCount, 0, sizeof(JqState.m_SemaphoreClientCount));
	JqState.m_ActiveSemaphores = 0;
	JqState.m_Attributes	   = *pAttr;
	JqState.nNumWorkers		   = pAttr->nNumWorkers;

	for(uint32_t i = 0; i < pAttr->nNumWorkers; ++i)
	{
		JQ_ASSERT(pAttr->WorkerOrderIndex[i] < pAttr->nNumPipeOrders); /// out of bounds pipe order index in attributes
		JqState.ThreadConfig[i] = pAttr->PipeOrder[pAttr->WorkerOrderIndex[i]];
	}

	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JqPipeOrder& C				 = JqState.ThreadConfig[i];
		uint8_t		 nNumActivePipes = 0;
		uint64_t	 PipeMask		 = 0;
		static_assert(JQ_NUM_PIPES < 64, "wont fit in 64bit mask");
		for(uint32_t j = 0; j < C.nNumPipes; ++j)
		{
			if(C.nPipes[j] != 0xff)
			{
				JqState.m_PipeList[i][nNumActivePipes++] = C.nPipes[j];
				JQ_ASSERT(C.nPipes[j] < JQ_NUM_PIPES);
				PipeMask |= 1llu << C.nPipes[j];
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.m_nNumPipes[i] = nNumActivePipes;
		int nSelectedSemaphore = -1;
		for(int j = 0; j < JqState.m_ActiveSemaphores; ++j)
		{
			if(JqState.m_SemaphoreMask[j] == PipeMask)
			{
				nSelectedSemaphore = j;
				break;
			}
		}
		if(-1 == nSelectedSemaphore)
		{
			JQ_ASSERT(JqState.m_ActiveSemaphores < JQ_MAX_SEMAPHORES);
			nSelectedSemaphore							= JqState.m_ActiveSemaphores++;
			JqState.m_SemaphoreMask[nSelectedSemaphore] = PipeMask;
			for(uint32_t j = 0; j < JQ_NUM_PIPES; ++j)
			{
				if(PipeMask & (1llu << j))
				{
					JQ_ASSERT(JqState.m_PipeNumSemaphores[j] < JQ_MAX_SEMAPHORES);
					JqState.m_PipeToSemaphore[j][JqState.m_PipeNumSemaphores[j]++] = (uint8_t)nSelectedSemaphore;
				}
			}
		}
		JQ_ASSERT(JqState.m_SemaphoreClientCount[nSelectedSemaphore] < JQ_MAX_SEMAPHORES);
		JqState.m_SemaphoreClients[nSelectedSemaphore][JqState.m_SemaphoreClientCount[nSelectedSemaphore]++] = (uint8_t)i;
		JqState.m_SemaphoreIndex[i]																			 = (uint8_t)nSelectedSemaphore;
	}

	for(uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
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
	JqState.nStop		  = 0;
	JqState.nTotalWaiting = 0;
	for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
	{
		JqState.Jobs[i].nStartedHandle	= 0;
		JqState.Jobs[i].nFinishedHandle = 0;
	}
	memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.nFreeJobs			   = JQ_NUM_JOBS;
	JqState.nNextHandle			   = 1;
	JqState.Stats.nNumFinished	   = 0;
	JqState.Stats.nNumLocks		   = 0;
	JqState.Stats.nNumSema		   = 0;
	JqState.Stats.nNumLocklessPops = 0;
	JqState.Stats.nNumWaitCond	   = 0;

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
	for(int i = 0; i < JqState.m_ActiveSemaphores; ++i)
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

void JqSetThreadPipeOrder(JqPipeOrder* pConfig)
{
	JQ_ASSERT(g_nJqNumQueues == 0); // its not supported to change this value, nor is it supported to set it on worker threads. set on init instead.
	uint32_t nNumActiveQueues = 0;
	JQ_ASSERT(pConfig->nNumPipes <= JQ_NUM_PIPES);
	for(uint32_t j = 0; j < pConfig->nNumPipes; ++j)
	{
		if(pConfig->nPipes[j] != 0xff)
		{
			g_JqQueues[nNumActiveQueues++] = pConfig->nPipes[j];
			JQ_ASSERT(pConfig->nPipes[j] < JQ_NUM_PIPES);
		}
	}
	g_nJqNumQueues = nNumActiveQueues;
}

void JqConsumeStats(JqStats* pStats)
{
	JqMutexLock lock(JqState.Mutex);
	*pStats						   = JqState.Stats;
	JqState.Stats.nNumAdded		   = 0;
	JqState.Stats.nNumFinished	   = 0;
	JqState.Stats.nNumAddedSub	   = 0;
	JqState.Stats.nNumFinishedSub  = 0;
	JqState.Stats.nNumCancelled	   = 0;
	JqState.Stats.nNumCancelledSub = 0;
	JqState.Stats.nNumLocks		   = 0;
	JqState.Stats.nNumSema		   = 0;
	JqState.Stats.nNumLocklessPops = 0;
	JqState.Stats.nNumWaitCond	   = 0;
	JqState.Stats.nNextHandle	   = JqState.nNextHandle;
}

void JqCheckFinished(uint64_t nJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint32_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	// JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].nFinishedHandle, nJob));
	// JQ_ASSERT(nJob == JqState.Jobs[nIndex].nStartedHandle);

	// JqJob* pEntry = &JqState.Jobs[nIndex];
	// if(0 == pEntry->nFirstChild && pEntry->nNumFinished == pEntry->nNumJobs)
	// {
	// 	uint16_t nParent = pEntry->nParent;
	// 	if(nParent)
	// 	{
	// 		uint64_t nParentHandle = JqDetachChild(nJob);
	// 		pEntry->nParent		   = 0;
	// 		JqCheckFinished(nParentHandle);
	// 	}
	// 	pEntry->nFinishedHandle = pEntry->nStartedHandle;
	// 	JQ_CLEAR_FUNCTION(pEntry->Function);

	// 	uint64_t Precondition	   = pEntry->nPreconditionFirst;
	// 	pEntry->nPreconditionFirst = 0;

	// 	while(Precondition != 0)
	// 	{
	// 		uint32_t nPreconditionIndex	   = Precondition % JQ_PIPE_BUFFER_SIZE;
	// 		JqJob*	 pStalled			   = &JqState.Jobs[nPreconditionIndex];
	// 		pStalled->nStalled			   = 0;
	// 		Precondition				   = pStalled->nPreconditionSibling;
	// 		pStalled->nPreconditionSibling = 0;

	// 		JqPriorityListAdd(nPreconditionIndex);
	// 	}

	// 	JqState.Stats.nNumFinished++;
	// 	// kick waiting threads.
	// 	int8_t nWaiters = pEntry->nWaiters;
	// 	if(nWaiters != 0)
	// 	{
	// 		JqState.Stats.nNumSema++;
	// 		JqState.WaitCond.NotifyAll();
	// 		pEntry->nWaiters	= 0;
	// 		pEntry->nWaitersWas = nWaiters;
	// 	}
	// 	else
	// 	{
	// 		pEntry->nWaitersWas = 0xff;
	// 	}
	// 	JqState.nFreeJobs++;
	// }
}

uint16_t JqIncrementStarted(uint64_t nJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	// JQ_ASSERT(JqState.Jobs[nIndex].nNumJobs > JqState.Jobs[nIndex].nNumStarted);
	// uint16_t nSubIndex = JqState.Jobs[nIndex].nNumStarted++;
	// if(JqState.Jobs[nIndex].nNumStarted == JqState.Jobs[nIndex].nNumJobs)
	// {
	// 	JqPriorityListRemove(nIndex);
	// }
	// return nSubIndex;
}
void JqIncrementFinished(uint64_t nJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
	// uint16_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	// JqState.Jobs[nIndex].nNumFinished++;
	// JqState.Stats.nNumFinishedSub++;
	// JqCheckFinished(nJob);
}

void JqAttachChild(uint64_t nParentJob, uint64_t nChildJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint16_t nParentIndex				   = nParentJob % JQ_PIPE_BUFFER_SIZE;
	// uint16_t nChildIndex				   = nChildJob % JQ_PIPE_BUFFER_SIZE;
	// uint16_t nFirstChild				   = JqState.Jobs[nParentIndex].nFirstChild;
	// JqState.Jobs[nChildIndex].nParent	   = nParentIndex;
	// JqState.Jobs[nChildIndex].nSibling	   = nFirstChild;
	// JqState.Jobs[nParentIndex].nFirstChild = nChildIndex;

	// JQ_ASSERT(JqState.Jobs[nParentIndex].nFinishedHandle != nParentJob);
	// JQ_ASSERT(JqState.Jobs[nParentIndex].nStartedHandle == nParentJob);
}

uint64_t JqDetachChild(uint64_t nChildJob)
{
	JQ_BREAK();
	// uint16_t nChildIndex  = nChildJob % JQ_PIPE_BUFFER_SIZE;
	// uint16_t nParentIndex = JqState.Jobs[nChildIndex].nParent;
	// JQ_ASSERT(JqState.Jobs[nChildIndex].nNumFinished == JqState.Jobs[nChildIndex].nNumJobs);
	// JQ_ASSERT(JqState.Jobs[nChildIndex].nNumFinished == JqState.Jobs[nChildIndex].nNumStarted);
	// if(0 == nParentIndex)
	// {
	// 	return 0;
	// }

	// JQ_ASSERT(nParentIndex != 0);
	// JQ_ASSERT(JqState.Jobs[nChildIndex].nFirstChild == 0);
	// uint16_t* pChildIndex = &JqState.Jobs[nParentIndex].nFirstChild;
	// JQ_ASSERT(JqState.Jobs[nParentIndex].nFirstChild != 0);
	// while(*pChildIndex != nChildIndex)
	// {
	// 	JQ_ASSERT(JqState.Jobs[*pChildIndex].nSibling != 0);
	// 	pChildIndex = &JqState.Jobs[*pChildIndex].nSibling;
	// }
	// JQ_ASSERT(pChildIndex);
	// *pChildIndex = JqState.Jobs[nChildIndex].nSibling;
	// return JqState.Jobs[nParentIndex].nStartedHandle;
}

// splits range evenly to jobs
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
	JqSelfStack[JqSelfPos].nJob		 = nJob;
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
	return JqSelfStack[JqSelfPos - 1].nJobIndex;
}

int JqGetNumWorkers()
{
	return JqState.nNumWorkers;
}

void JqContextRun(JqTransfer T)
{
	JqJobStack* pJobData = (JqJobStack*)T.data;
	JqState.Jobs[pJobData->nExternalId].Function(pJobData->nBegin, pJobData->nEnd);
	jq_jump_fcontext(T.fctx, (void*)447);
	JQ_BREAK();
}

JqJobStackList& JqGetJobStackList(uint32_t nFlags)
{
	bool bSmall = 0 != (nFlags & JQ_JOBFLAG_SMALL_STACK);
	return bSmall ? JqState.m_StackSmall : JqState.m_StackLarge;
}

void JqRunInternal(uint32_t nWorkIndex, int nBegin, int nEnd)
{
	JQ_ASSERT(JqState.Jobs[nWorkIndex].nStalled == 0);

	if(JQ_INIT_USE_SEPERATE_STACK == (JqState.m_Attributes.Flags & JQ_INIT_USE_SEPERATE_STACK))
	{
		uint32_t	nFlags	   = JqState.Jobs[nWorkIndex].nJobFlags;
		bool		bSmall	   = 0 != (nFlags & JQ_JOBFLAG_SMALL_STACK);
		uint32_t	nStackSize = bSmall ? JqState.m_Attributes.nStackSizeSmall : JqState.m_Attributes.nStackSizeLarge;
		JqJobStack* pJobData   = JqAllocStack(JqGetJobStackList(nFlags), nStackSize, nFlags);
		void*		pVerify	   = g_pJqJobStacks;
		JQ_ASSERT(pJobData->pLink == nullptr);
		pJobData->pLink		  = g_pJqJobStacks;
		pJobData->nBegin	  = nBegin;
		pJobData->nEnd		  = nEnd;
		pJobData->nExternalId = nWorkIndex;
		g_pJqJobStacks		  = pJobData;
		pJobData->pContextJob = jq_make_fcontext(pJobData->StackTop(), pJobData->StackSize(), JqContextRun);
		JqTransfer T		  = jq_jump_fcontext(pJobData->pContextJob, (void*)pJobData);
		JQ_ASSERT(T.data == (void*)447);
		g_pJqJobStacks	= pJobData->pLink;
		pJobData->pLink = nullptr;
		JQ_ASSERT(pVerify == g_pJqJobStacks);
		JQ_ASSERT(pJobData->GUARD[0] == 0xececececececececll);
		JQ_ASSERT(pJobData->GUARD[1] == 0xececececececececll);
		JqFreeStack(JqGetJobStackList(nFlags), pJobData);
	}
	else
	{
		JqState.Jobs[nWorkIndex].Function(nBegin, nEnd);
	}
}

void JqExecuteJob(uint64_t nJob, uint16_t nSubIndex)
{
	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
	JQ_ASSERT_NOT_LOCKED(JqState.Mutex);

	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);

	JqSelfPush(nJob, nSubIndex);
	{
		uint16_t nWorkIndex = nJob % JQ_PIPE_BUFFER_SIZE;
		uint16_t nNumJobs	= JqState.Jobs[nWorkIndex].NumJobs;
		int		 nRange		= JqState.Jobs[nWorkIndex].Range;
		int		 nFraction	= nRange / nNumJobs;
		int		 nRemainder = nRange - nFraction * nNumJobs;
		int		 nStart		= JqGetRangeStart(nSubIndex, nFraction, nRemainder);
		int		 nEnd		= JqGetRangeStart(nSubIndex + 1, nFraction, nRemainder);
		JqRunInternal(nWorkIndex, nStart, nEnd);
	}
	JqSelfPop(nJob);
}

uint16_t JqTakeJob(uint16_t* pSubIndex, uint32_t nNumQueues, uint8_t* pQueues)
{
	const uint32_t nCount = nNumQueues ? nNumQueues : JQ_NUM_PIPES;
	for(uint32_t i = 0; i < nCount; i++)
	{
		uint8_t	 nQueue = nNumQueues : pQueues[i] : i;
		uint16_t nIndex = JqQueuePop(nQueue, pSubIndex);
		if(nIndex)
			return nIndex;
	}
	return 0;
}

// JQ_ASSERT_LOCKED(JqState.Mutex);
// if(nNumPrio)
// {
// 	for(uint32_t i = 0; i < nNumPrio; i++)
// 	{
// 		uint16_t nIndex = JqState.nPrioListHead[pPrio[i]];
// 		if(nIndex)
// 		{
// 			JQ_ASSERT(JqState.Jobs[nIndex].nStalled == 0);
// 			*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
// 			return nIndex;
// 		}
// 	}
// }
// else
// {
// 	for(int i = 0; i < JQ_NUM_PIPES; ++i)
// 	{
// 		uint16_t nIndex = JqState.nPrioListHead[i];
// 		if(nIndex)
// 		{
// 			JQ_ASSERT(JqState.Jobs[nIndex].nStalled == 0);
// 			*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
// 			return nIndex;
// 		}
// 	}
// }
return 0;
}

// #ifdef JQ_ASSERT_SANITY
// void JqTagChildren(uint16_t nRoot)
// {
// 	for(int i = 1; i < JQ_PIPE_BUFFER_SIZE; ++i)
// 	{
// 		JQ_ASSERT(JqState.Jobs[i].nTag == 0);
// 		if(nRoot == i)
// 		{
// 			JqState.Jobs[i].nTag = 1;
// 		}
// 		else
// 		{
// 			int nParent = JqState.Jobs[i].nParent;
// 			while(nParent)
// 			{
// 				if(nParent == nRoot)
// 				{
// 					JqState.Jobs[i].nTag = 1;
// 					break;
// 				}
// 				nParent = JqState.Jobs[nParent].nParent;
// 			}
// 		}
// 	}
// }
// void JqCheckTagChildren(uint16_t nRoot)
// {
// 	for(int i = 1; i < JQ_PIPE_BUFFER_SIZE; ++i)
// 	{
// 		JQ_ASSERT(JqState.Jobs[i].nTag == 0);
// 		bool bTagged = false;
// 		if(nRoot == i)
// 			bTagged = false;
// 		else
// 		{
// 			int nParent = JqState.Jobs[i].nParent;
// 			while(nParent)
// 			{
// 				if(nParent == nRoot)
// 				{
// 					bTagged = true;
// 					break;
// 				}
// 				nParent = JqState.Jobs[nParent].nParent;
// 			}
// 		}
// 		JQ_ASSERT(bTagged == (1 == JqState.Jobs[i].nTag));
// 	}
// }

// void JqLoopChildren(uint16_t nRoot)
// {
// 	int nNext = JqState.Jobs[nRoot].nFirstChild;
// 	while(nNext != nRoot && nNext)
// 	{
// 		while(JqState.Jobs[nNext].nFirstChild)
// 			nNext = JqState.Jobs[nNext].nFirstChild;
// 		JQ_ASSERT(JqState.Jobs[nNext].nTag == 1);
// 		JqState.Jobs[nNext].nTag = 0;
// 		if(JqState.Jobs[nNext].nSibling)
// 			nNext = JqState.Jobs[nNext].nSibling;
// 		else
// 		{
// 			// search up
// 			nNext = JqState.Jobs[nNext].nParent;
// 			while(nNext != nRoot)
// 			{
// 				JQ_ASSERT(JqState.Jobs[nNext].nTag == 1);
// 				JqState.Jobs[nNext].nTag = 0;
// 				if(JqState.Jobs[nNext].nSibling)
// 				{
// 					nNext = JqState.Jobs[nNext].nSibling;
// 					break;
// 				}
// 				else
// 				{
// 					nNext = JqState.Jobs[nNext].nParent;
// 				}
// 			}
// 		}
// 	}

// 	JQ_ASSERT(JqState.Jobs[nRoot].nTag == 1);
// 	JqState.Jobs[nRoot].nTag = 0;
// }
// #endif

// Depth first. Once a node is visited all child nodes have been visited
uint16_t JqTreeIterate(uint64_t nJob, uint16_t nCurrent)
{

	JQ_BREAK();
	// JQ_ASSERT(nJob);
	// uint16_t nRoot = nJob % JQ_PIPE_BUFFER_SIZE;
	// if(nRoot == nCurrent)
	// {
	// 	while(JqState.Jobs[nCurrent].nFirstChild)
	// 		nCurrent = JqState.Jobs[nCurrent].nFirstChild;
	// }
	// else
	// {
	// 	// once here all child nodes _have_ been processed.
	// 	if(JqState.Jobs[nCurrent].nSibling)
	// 	{
	// 		nCurrent = JqState.Jobs[nCurrent].nSibling;
	// 		while(JqState.Jobs[nCurrent].nFirstChild) // child nodes first.
	// 			nCurrent = JqState.Jobs[nCurrent].nFirstChild;
	// 	}
	// 	else
	// 	{
	// 		nCurrent = JqState.Jobs[nCurrent].nParent;
	// 	}
	// }
	// return nCurrent;
}

// this code is O(n) where n is the no. of child nodes.
// I wish this could be written in a simpler way
uint16_t JqTakeChildJob(uint64_t nJob, uint16_t* pSubIndexOut)
{
	JQ_BREAK();

	// 	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeChildJob", 0xff);
	// 	JQ_ASSERT_LOCKED(JqState.Mutex);
	// #if JQ_ASSERT_SANITY
	// 	{
	// 		// verify that the child iteration is sane
	// 		for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
	// 		{
	// 			JqState.Jobs[i].nTag = 0;
	// 		}
	// 		JqTagChildren(nJob % JQ_PIPE_BUFFER_SIZE);
	// 		uint16_t nIndex		= nJob % JQ_PIPE_BUFFER_SIZE;
	// 		uint16_t nRootIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	// 		do
	// 		{
	// 			nIndex = JqTreeIterate(nJob, nIndex);
	// 			JQ_ASSERT(JqState.Jobs[nIndex].nTag == 1);
	// 			JqState.Jobs[nIndex].nTag = 0;
	// 		} while(nIndex != nRootIndex);
	// 		for(int i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
	// 		{
	// 			JQ_ASSERT(JqState.Jobs[i].nTag == 0);
	// 		}
	// 	}
	// #endif

	// 	uint16_t nRoot	= nJob % JQ_PIPE_BUFFER_SIZE;
	// 	uint16_t nIndex = nRoot;

	// 	do
	// 	{
	// 		nIndex = JqTreeIterate(nJob, nIndex);
	// 		JQ_ASSERT(nIndex);

	// 		if(JqState.Jobs[nIndex].nNumStarted < JqState.Jobs[nIndex].nNumJobs)
	// 		{
	// 			JQ_ASSERT(JqState.Jobs[nIndex].nStalled == 0);
	// 			*pSubIndexOut = JqIncrementStarted(JqState.Jobs[nIndex].nStartedHandle);
	// 			return nIndex;
	// 		}
	// 	} while(nIndex != nRoot);

	return 0;
}

bool JqExecuteOne()
{
	return JqExecuteOne(g_JqQueues, (uint8_t)g_nJqNumQueues);
}

bool JqExecuteOne(uint8_t nPipe)
{
	return JqExecuteOne(&nPipe, 1);
}

bool JqExecuteOne(uint8_t* nQueues, uint8_t nNumQueues)
{
	uint16_t nSubIndex = 0;
	uint16_t nWork	   = JqTakeJob(&nSubIndex, nNumQueues, nQueues);
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	nWork = JqTakeJob(&nSubIndex, nNumPipes, pPipes);
	// }
	if(!nWork)
		return false;
	JqExecuteJob(JqState.Jobs[nWork].StartedHandle, nSubIndex);
	JqIncrementFinished(nWork, JqState.Jobs[nWork].nStartedHandle);
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.Jobs[nWork].nStartedHandle);
	// }
	return true;
}

void JqWorker(int nThreadId)
{

	uint8_t* pQueues	= JqState.m_PipeList[nThreadId];
	uint32_t nNumQueues = JqState.m_nNumPipes[nThreadId];
	g_nJqNumQueues		= nNumQueues; // even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqQueues, pQueues, nNumQueues);
	int nSemaphoreIndex = JqState.m_SemaphoreIndex[nThreadId];

#if MICROPROFILE_ENABLED
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker) - 1, "JqWorker %d %08llx", nThreadId, JqState.m_SemaphoreMask[nSemaphoreIndex]);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.nStop)
	{
		do
		{
			uint16_t nSubIndex = 0;
			uint16_t nWork	   = JqTakeJob(&nSubIndex, nNumQueues, pQueues);
			if(!nWork)
				break;
			JqExecuteJob(JqState.Jobs[nWork].nStartedHandle, nSubIndex);
			JqIncrementFinished(nWork, JqState.Jobs[nWork].nStartedHandle);

			// {
			// 	JqMutexLock lock(JqState.Mutex);
			// 	if(nWork)
			// 	{
			// 		JqIncrementFinished(JqState.Jobs[nWork].nStartedHandle);
			// 	}
			// 	nSubIndex = 0;
			// 	nWork	  = JqTakeJob(&nSubIndex, nNumPipes, pPipes);
			// }
			// if(!nWork)
			// {
			// 	break;
			// }
			// JqExecuteJob(JqState.Jobs[nWork].nStartedHandle, nSubIndex);
		} while(1);

		JqState.Semaphore[nSemaphoreIndex].Wait();
	}
#ifdef JQ_MICROPROFILE
	MicroProfileOnThreadExit();
#endif
}
uint64_t JqNextHandle(uint64_t nJob)
{
	nJob++;
	if(0 == (nJob % JQ_PIPE_BUFFER_SIZE))
	{
		nJob++;
	}
	return nJob;
}
uint64_t JqClaimHandle()
{
	// JQ_ASSERT_LOCKED(JqState.Mutex);

#if 0
	while(!JqState.nFreeJobs)
	{
		if(JqSelfPos < JQ_MAX_JOB_STACK)
		{
			JQ_MICROPROFILE_SCOPE("AddExecute", 0xff); // if this starts happening the job queue size should be increased..
			uint16_t nSubIndex = 0;
			uint16_t nIndex	   = JqTakeJob(&nSubIndex, 0, nullptr);
			if(nIndex)
			{
				Lock.Unlock();
				JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
				Lock.Lock();
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			}
		}
		else
		{
			JQ_BREAK(); // out of job queue space. increase JQ_PIPE_BUFFER_SIZE or create fewer jobs
		}
	}
	JQ_ASSERT(JqState.nFreeJobs != 0);
	uint64_t nNextHandle = JqState.nNextHandle;
	uint16_t nCount		 = 0;
	while(JqPendingJobs(nNextHandle))
	{
		nNextHandle = JqNextHandle(nNextHandle);
		JQ_ASSERT(nCount++ < JQ_PIPE_BUFFER_SIZE);
	}
	JqState.nNextHandle = JqNextHandle(nNextHandle);
	JqState.nFreeJobs--;
	return nNextHandle;
#endif
	JQ_ASSERT(JqState.FreeJobs);

	uint64_t h;
	// claim a handle atomically.
	JqJob* pJob = nullptr;
	while(1)
	{
		h = JqState.NextHandle.fetch_add(1);

		if(h == 0) // slot 0 is reserved
			continue;

		uint16_t index = h % JQ_PIPE_BUFFER_SIZE;
		pJob		   = JqState.Jobs[index];

		uint64_t finished = pJob.FinishedHandle.load();
		uint64_t claimed  = pJob.ClaimedHandle.load();

		if(finished != claimed) // if the last claimed job is not yet done, we cant use it
			continue;

		if(job.ClaimedHandle.compare_exchange_strong(claimed, h))
			break;
	}
	// reset header
	pJob->PendingCount		= 0;
	pJob->PreconditionCount = 1;
	pJob->Locked			= false;
	pJob->NumJobs			= 0;
	pJob->Range				= 0;
	pJob->JobFlags			= JQ_JOBFLAG_UNINITIALIZED;
	pJob->Waiters			= 0;

	return h;
}

JQ_API void JqSpawn(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nWaitFlag)
{
	uint64_t nJob = JqAdd(JobFunc, nPrio, nNumJobs, nRange);
	JqWait(nJob, nWaitFlag);
}

bool JqCancel(uint64_t nJob)
{

	// todo: implement
	JQ_BREAK();
	// JqMutexLock Lock(JqState.Mutex);
	// uint16_t	nIndex = nJob % JQ_PIPE_BUFFER_SIZE;

	// JqJob* pEntry = &JqState.Jobs[nIndex];
	// if(pEntry->nStartedHandle != nJob)
	// 	return false;
	// if(pEntry->nNumStarted != 0)
	// 	return false;
	// uint32_t nNumJobs = pEntry->nNumJobs;
	// pEntry->nNumJobs  = 0;

	// JqPriorityListRemove(nIndex);
	// JqCheckFinished(nJob);
	// JQ_ASSERT(JqIsDone(nJob));

	// JqState.Stats.nNumCancelled++;
	// JqState.Stats.nNumCancelledSub += nNumJobs;
	return true;
}
void JqIncPrecondtion(uint64_t Handle, int Count)
{
	uint16_t Index = Handle % JQ_PIPE_BUFFER_SIZE;
	JqJob*	 pJob  = &JqState.Jobs[Index];

	uint64_t Before = pJob->PreconditionCount.fetch_add(Count);
	// Adding preconditions is only allowed when the job has been reserved, in which case it'll have a count of 1 or larger
	// If we don't do it this way, jobs may be picked up before having their preconditions added.
	JQ_ASSERT(Before > 0);
}

void JqDecPrecondtion(uint64_t Handle, int Count)
{
	uint16_t Index = Handle % JQ_PIPE_BUFFER_SIZE;
	JqJob*	 pJob  = &JqState.Jobs[Index];

	uint64_t Before = pJob->PreconditionCount.fetch_add(-Count);
	if(Before - Count == 0)
	{
		uint8_t			  Queue = pJob->Queue;
		JqSingleMutexLock L(JqState.QueueMutex[Queue]);
		JqQueueAdd(Handle, Queue);
	}
}

JqMutex& JqGetQueudeMutex(uint16_t QueueIndex){ return JqState.MutexQueue[JobIndex % JOB_MUTEX_COUNT]

} JqMutex& JqGetJobMutex(uint16_t JobIndex)
{
	return JqState.MutexJob[JobIndex % JOB_MUTEX_COUNT]
}

void JqQueuePush(uint8_t nQueue, uint16_t nJobIndex)
{
	JQ_ASSERT(nJobIndex < JQ_PIPE_BUFFER_SIZE);
	JQ_ASSERT(nQueue < JQ_NUM_PIPES);
	JqJob&	 Job   = JqState.Jobs[nJobIndex];
	JqQueue& Queue = JqState.Queues[nQueue];

	JQ_ASSERT(Job.Precondition == 0);
	JQ_ASSERT(Jobs.nPrio == nQueue);
	uint64_t Started  = Job.StartedHandle;
	uint64_t Finished = Job.FinishedHandle;
	uint64_t Claimed  = Job.ClaimedHandle;
	JQ_ASSERT(Started == FinishedHandle);
	JQ_ASSERT(JQ_LT_WRAP(Started, Claimed));
	JQ_ASSERT(JQ_LT_WRAP(Fini, Claimed));

	JqSingleMutexLock L(Queue.Mutex);
	JqAssert(Job.LinkPrev == 0);
	JqAssert(Job.LinkNext == 0);

	Job.LinkPrev = Queue.LinkTail;
	if(Queue.LinkTail)
	{
		JqState.Jobs[Queue.LinkTail].LinkNext = nJobIndex;
	}
	else
	{
		JQ_ASSERT(Queue.LinkHead == 0);
		Queue.LinkHead = Queue.LinkTail = nJobIndex;
	}
}

uint16_t JqQueuePop(uint8_t Queue, uint16_t* OutSubJob)
{
	JQ_ASSERT(nQueue < JQ_NUM_PIPES);
	JqQueue&		  Queue = JqState.Queues[nQueue];
	JqSingleMutexLock L(Queue.Mutex);
	uint16_t		  JobIndex = Queue.LinkHead;
	if(JobIndex)
	{
		JQ_ASSERT(JobIndex < JQ_PIPE_BUFFER_SIZE);
		JqJob& Job		= JqState.Jobs[JobIndex];
		int	   SubIndex = --Job.PendingCount;
		if(SubIndex == 0)
		{
			Queue.LinkHead = Job.LinkNext;
			if(JobIndex == Queue.LinkTail)
			{
				JQ_ASSERT(Queue.LinkHead == 0);
				Queue.LinkTail = 0;
			}
		}
		*OutSubJob = Job.NumJobs - 1 - SubIndex; // do it in order.
	}
	return JobIndex;
}

void JqAddPrecondition(uint64_t Handle, uint64_t Precondition)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint16_t nIndex = Handle % JQ_PIPE_BUFFER_SIZE;
	// if(JqIsDone(Precondition))
	// {
	// 	JqJob* pEntry	 = &JqState.Jobs[nIndex];
	// 	pEntry->nStalled = 0;
	// 	JqPriorityListAdd(nIndex);
	// }
	// else
	// {
	// 	uint16_t nPreconditionIndex	  = Precondition % JQ_PIPE_BUFFER_SIZE;
	// 	JqJob*	 pEntry				  = &JqState.Jobs[nIndex];
	// 	JqJob*	 pPreEntry			  = &JqState.Jobs[nPreconditionIndex];
	// 	pEntry->nPreconditionSibling  = pPreEntry->nPreconditionFirst;
	// 	pPreEntry->nPreconditionFirst = Handle;
	// }
}

uint64_t JqAddInternal(uint64_t ReservedHandle, JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nJobFlags, uint64_t Precondition)
{
	// Add:
	//	* Allocate header (inc counter)
	//	* Update header as reserved
	//	* Fill in header
	//  * if there is a parent, increment its pending count by no. of jobs
	//	* lock pipe mtx
	//  	* add to pipe
	// 	* unlock
	//

	uint64_t nParent = 0 != (nJobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf();
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

	uint64_t nHandle = 0;

	bool bAdded = false;
	{
		// JqMutexLock Lock(JqState.Mutex);
		if(ReservedHandle)
		{
			JQ_ASSERT(nPrio == 0xff);
			nHandle = ReservedHandle;
		}
		else
		{
			nHandle = JqClaimHandle();
		}

		uint16_t nIndex = nHandle % JQ_PIPE_BUFFER_SIZE;
		JqJob*	 pJob	= &JqState.Jobs[nIndex];

		if(ReservedHandle)
		{
			JQ_ASSERT(pJob->nStartedHandle == ReservedHandle);
			JQ_ASSERT(pJob->nPrio != 0xff);
		}
		else
		{
			JQ_ASSERT(pJob->PreconditionCount == 1);
			JQ_ASSERT(nPrio < JQ_NUM_PIPES);
			pJob->nPrio	   = nPrio;
			pJob->nWaiters = 0;
		}

		JQ_ASSERT(pJob->ClaimedHandle.load() == nHandle);
		JQ_ASSERT(JQ_LT_WRAP(pEntry->nFinishedHandle, nHandle));

		JQ_ASSERT(nNumJobs <= 0xffff);

		pJob->PendingStart	= nNumJobs;
		pJob->PendingFinish = nNumJobs;
		pJob->PreconditionCount++;
		pJob->nRange	= 0;
		pJob->nJobFlags = nFlags;

		// increment parents finish count
		// uint64_t nParentHandle = nParent;
		// nParentHandle % JQ_PIPE_BUFFER_SIZE;
		pEntry->nParent = nParent;

		// a parent is by definition -never- finished when we add a child, so we can just atomically one
		JqJob* pParentJob = &JqState.Jobs[nParent % JQ_PIPE_BUFFER_SIZE];
		pParentJob->PendingFinish.fetch_add(1);

		pEntry->Function = JobFunc;

		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += nNumJobs;
		if(Precondition)
		{
			JqAddPrecondition(Handle, Precondition);
		}

		// Decrementing preconditions automatically add to a queue when reaching 0
		JQ_ASSERT(pJob->PreconditionCount > 0);
		JqDecPrecondtionCount(Handle, 1);
	}
	return nHandle;
}

uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange, uint32_t nJobFlags)
{
	return JqAddInternal(0, JobFunc, nPrio, nNumJobs, nRange, nJobFlags, 0);
}

// add reserved
uint64_t JqAddReserved(uint64_t ReservedHandle, JqFunction JobFunc, int nNumJobs, int nRange, uint32_t nJobFlags)
{
	return JqAddInternal(ReservedHandle, JobFunc, 0xff, nNumJobs, nRange, nJobFlags, 0);
}

// add successor
uint64_t JqAddSuccessor(uint64_t Precondition, JqFunction JobFunc, uint8_t nPipe, int nNumJobs, int nRange, uint32_t nJobFlags)
{
	return JqAddInternal(0, JobFunc, nPipe, nNumJobs, nRange, nJobFlags, Precondition);
}

// Reserve a Job slot. this allows you to wait on work added later
uint64_t JqReserve(uint8_t nPipe)
{
	JQ_ASSERT(nPipe < JQ_NUM_PIPES);

	uint64_t nNextHandle = JqClaimHandle();
	uint16_t nIndex		 = nNextHandle % JQ_PIPE_BUFFER_SIZE;

	JqJob* pEntry = &JqState.Jobs[nIndex];

	JQ_ASSERT(JQ_LE_WRAP(pEntry->nFinishedHandle, nNextHandle));
	JQ_ASSERT(pJob->PreconditionCount == 1);
	JQ_ASSERT(pJob->PendingCount == 0);
	// pJob->PendingCount = 1;
	// pJob->PreconditionCount++;
	// pJob->nRange = 0;

	// pEntry->nStartedHandle = nNextHandle;
	// pEntry->nNumJobs	   = 1;
	// pEntry->nNumStarted	   = 1;
	// pEntry->nNumFinished   = 0;
	// pEntry->nRange		   = 0;
	// // hmm, consider this:
	// // uint64_t nParentHandle = JqSelf();
	// // pEntry->nParent = nParentHandle % JQ_PIPE_BUFFER_SIZE;
	// // if(pEntry->nParent)
	// // {
	// // 	JqAttachChild(nParentHandle, nNextHandle);
	// // }
	JQ_CLEAR_FUNCTION(pEntry->Function);
	pEntry->nPrio	 = nPipe;
	pEntry->nWaiters = 0;

	return nNextHandle;
}
// Mark reservation as finished, without actually executing any jobs.
void JqCloseReserved(uint64_t Handle)
{
	JqDecPrecondtion(Handle, 1);
}

void JqDump()
{
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t Index			= nJob % JQ_PIPE_BUFFER_SIZE;
	uint64_t FinishedHandle = JqState.Jobs[Index].FinishedHandle;
	uint64_t StartedHandle	= JqState.Jobs[Index].StartedHandle;
	JQ_ASSERT(JQ_LE_WRAP(FinishedHandle, StartedHandle));
	int64_t nDiff = (int64_t)(FinishedHandle - nJob);
	JQ_ASSERT((nDiff >= 0) == JQ_LE_WRAP(nJob, FinishedHandle));
	return JQ_LE_WRAP(nJob, FinishedHandle);
}

bool JqIsDoneExt(uint64_t nJob, uint32_t nWaitFlags)
{
	bool bIsDone = JqIsDone(nJob);
	if(!bIsDone && 0 != (nWaitFlags & JQ_WAITFLAG_IGNORE_CHILDREN))
	{
		uint64_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
		return JqState.Jobs[nIndex].nNumJobs == JqState.Jobs[nIndex].nNumFinished;
	}
	return bIsDone;
}

bool JqPendingJobs(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_PIPE_BUFFER_SIZE;
	JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].nFinishedHandle, JqState.Jobs[nIndex].nStartedHandle));
	return JqState.Jobs[nIndex].FinishedHandle != JqState.Jobs[nIndex].ClaimedHandle;
}

void JqWaitAll()
{
	JQ_BREAK();
	// uint16_t nIndex = 0;
	// while(JqState.nFreeJobs != JQ_NUM_JOBS)
	// {
	// 	uint16_t nSubIndex = 0;
	// 	{
	// 		JqMutexLock lock(JqState.Mutex);
	// 		if(nIndex)
	// 		{
	// 			JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
	// 		}
	// 		nIndex = JqTakeJob(&nSubIndex, 0, nullptr);
	// 	}
	// 	if(nIndex)
	// 	{
	// 		JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
	// 	}
	// 	else
	// 	{
	// 		JQ_USLEEP(1000);
	// 	}
	// }
	// if(nIndex)
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
	// }
}

void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	uint16_t nIndex = 0;
	if(JqIsDone(nJob))
	{
		return;
	}
	while(!JqIsDoneExt(nJob, nWaitFlag))
	{

		uint16_t nSubIndex = 0;
		if((nWaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag))
				return;
			nIndex = JqTakeChildJob(nJob, &nSubIndex);
			if(!nIndex)
			{
				nIndex = JqTakeJob(&nSubIndex, g_nJqNumQueues, g_JqQueues);
			}
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag))
				return;
			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		else if(0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
			if(JqIsDoneExt(nJob, nWaitFlag))
				return;
			nIndex = JqTakeJob(&nSubIndex, g_nJqNumQueues, g_JqQueues);
		}
		else
		{
			JQ_BREAK();
		}
		if(!nIndex)
		{
			JQ_ASSERT(0 != (nWaitFlag & (JQ_WAITFLAG_SLEEP | JQ_WAITFLAG_BLOCK | JQ_WAITFLAG_SPIN)));
			if(nWaitFlag & JQ_WAITFLAG_SPIN)
			{
				uint64_t nTick			 = JqTick();
				uint64_t nTicksPerSecond = JqTicksPerSecond();
				do
				{
					uint32_t result = 0;
					for(uint32_t i = 0; i < 1000; ++i)
					{
						result |= i << (i & 7); // do something.. whatever
					}
					JqSpinloop |= result; // write it somewhere so the optimizer can't remote it
				} while((1000000ull * (JqTick() - nTick)) / nTicksPerSecond < nUsWaitTime);
			}
			else if(nWaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(nUsWaitTime);
			}
			else
			{
				JqMutexLock lock(JqState.Mutex);
				if(JqIsDoneExt(nJob, nWaitFlag))
				{
					return;
				}
				uint16_t nJobIndex = nJob % JQ_PIPE_BUFFER_SIZE;
				JqState.Jobs[nJobIndex].nWaiters++;
				JqState.Stats.nNumWaitCond++;
				JqState.WaitCond.Wait(JqState.Mutex);
				JqState.Jobs[nJobIndex].nWaiters--;
			}
		}
		else
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}
	}
	if(nIndex)
	{
		JqMutexLock lock(JqState.Mutex);
		JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);
	}
}

void JqExecuteChildren(uint64_t nJob)
{
	JQ_BREAK();
#if 0
	if(JqIsDone(nJob))
	{
		return;
	}

	uint16_t nIndex = 0;
	do
	{
		uint16_t nSubIndex = 0;
		{
			JqMutexLock lock(JqState.Mutex);
			if(nIndex)
				JqIncrementFinished(JqState.Jobs[nIndex].nStartedHandle);

			if(JqIsDone(nJob))
				return;

			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		if(nIndex)
		{
			JqExecuteJob(JqState.Jobs[nIndex].nStartedHandle, nSubIndex);
		}

	} while(nIndex);
#endif
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
	JQ_BREAK(); // todo
	return 0;
}

uint64_t JqGroupBegin(uint8_t nPriority)
{
	uint64_t nNextHandle = JqClaimHandle();
	uint16_t nIndex		 = nNextHandle % JQ_PIPE_BUFFER_SIZE;
	JqJob*	 pEntry		 = &JqState.Jobs[nIndex];

	JQ_ASSERT(JQ_LE_WRAP(pEntry->nFinishedHandle, nNextHandle));
	pEntry->nStartedHandle = nNextHandle;

	uint64_t nParentHandle = JqSelf();
	pEntry->nParent		   = nParentHandle % JQ_PIPE_BUFFER_SIZE;

	if(pEntry->nParent)
	{
		JqAttachChild(nParentHandle, nNextHandle);
	}
	JQ_CLEAR_FUNCTION(pEntry->Function);
	pEntry->nPrio = 0xff;
	JqSelfPush(nNextHandle, 0);
	return nNextHandle;
}

void JqGroupEnd()
{
	uint64_t nJob = JqSelf();
	JqSelfPop(nJob);

	JqDecPrecondtion(nJob, 1);

	// JqMutexLock lock(JqState.Mutex);
	// JqIncrementFinished(nJob);
}

uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos - 1].nJob : 0;
}

// void JqPriorityListAdd(uint16_t nIndex)
// {
// 	JqJob& Job = JqState.Jobs[nIndex];
// 	JQ_ASSERT(Jobs.nStalled == 0);
// 	uint8_t nPrio = Jobs.nPrio;
// 	JqAssert(Job.Link.nPrev == 0);
// 	JqAssert(Job.Link.nNext == 0);
// 	JqJobLink*		List = &Pipes.Link;
// 	JqJobLinkHelper Helper;
// 	do
// 	{
// 		Helper.nValue	   = List.load();
// 		uint64_t nOriginal = Helper.nValue;
// 		if(Helper.nNext == 0)
// 		{
// 			JQ_ASSERT(Helper.nPrev == 0);
// 			JQ_ASSERT(Helper.nStarted == 0);
// 			Helper.nRemaining = Job.nNumJobs;
// 			Helper.nNext	  = nIndex;
// 			Helper.nPrev	  = nIndex;
// 			Job.Link.nNext	  = 0;
// 			Job.Link.nPrev	  = 0;
// 			Job.Link.nVersion = 1;
// 		}
// 		else
// 		{
// 			Job.Link.nNext = 0;
// 			Job.Link.nPrev = Helper.nPrev;
// 			Helper.nPrev   = nIndex;
// 		}
// 		Helper.nVersion++;

// 		// uint16_t nNext;	   // next / head index
// 		// uint16_t nPrev;	   // prev / tail index
// 		// uint16_t nRemaining; // no of jobs started
// 		// uint16_t nVersion; // rolling counter to prevent accidental atomics.

// 	} while(!List.compare_exchange_weak(nOriginal, Helper.nValue));

// 	// // JQ_ASSERT(JqState.Jobs[nIndex].nLinkNext == 0);
// 	// // JQ_ASSERT(JqState.Jobs[nIndex].nLinkPrev == 0);
// 	// uint16_t nTail = JqState.nPrioListTail[nPrio];
// 	// if(nTail != 0)
// 	// {
// 	// 	JQ_ASSERT(JqState.Jobs[nTail].nLinkNext == 0);
// 	// 	JqState.Jobs[nTail].nLinkNext  = nIndex;
// 	// 	JqState.Jobs[nIndex].nLinkPrev = nTail;
// 	// 	JqState.Jobs[nIndex].nLinkNext = 0;
// 	// 	JqState.nPrioListTail[nPrio]   = nIndex;
// 	// }
// 	// else
// 	// {
// 	// 	JQ_ASSERT(JqState.nPrioListHead[nPrio] == 0);
// 	// 	JqState.nPrioListHead[nPrio]   = nIndex;
// 	// 	JqState.nPrioListTail[nPrio]   = nIndex;
// 	// 	JqState.Jobs[nIndex].nLinkNext = 0;
// 	// 	JqState.Jobs[nIndex].nLinkPrev = 0;
// 	// }
// }
// void JqPriorityListRemove(uint16_t nIndex)
// {
// 	uint8_t	 nPrio				   = JqState.Jobs[nIndex].nPrio;
// 	uint16_t nNext				   = JqState.Jobs[nIndex].nLinkNext;
// 	uint16_t nPrev				   = JqState.Jobs[nIndex].nLinkPrev;
// 	JqState.Jobs[nIndex].nLinkNext = 0;
// 	JqState.Jobs[nIndex].nLinkPrev = 0;

// 	if(nNext != 0)
// 	{
// 		JqState.Jobs[nNext].nLinkPrev = nPrev;
// 	}
// 	else
// 	{
// 		JQ_ASSERT(JqState.nPrioListTail[nPrio] == nIndex);
// 		JqState.nPrioListTail[nPrio] = nPrev;
// 	}
// 	if(nPrev != 0)
// 	{
// 		JqState.Jobs[nPrev].nLinkNext = nNext;
// 	}
// 	else
// 	{
// 		JQ_ASSERT(JqState.nPrioListHead[nPrio] == nIndex);
// 		JqState.nPrioListHead[nPrio] = nNext;
// 	}
// }
