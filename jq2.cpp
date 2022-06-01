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
//  TODO:
// 		* colors
//		* fix manymutex lock (lockless take-job)
//		* multidep
//		peeking
//		child-wait
//		* pop any job
//		cancel job
//
//		upgrade microprofile
//		switch to ng
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
// Release Reserved  (can be used to do barriers)
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

#define JQ_NUM_JOBS (JQ_JOB_BUFFER_SIZE - 1)
#define JQ_JOBFLAG_UNINITIALIZED 0x80
#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS
#define JQ_NUM_LOCKS 32
#define JQ_LOCKLESS_POP 1
#define JQ_LOCKLESS_PEEK_COUNT 0 // How many jobs do we allow peeking ahead? (Only for JQ_LOCKLESS_POP == 2)

struct JqMutexLock;

void				 JqWorker(int ThreadId);
void				 JqQueuePush(uint8_t Queue, uint64_t Handle);
uint16_t			 JqQueuePop(uint8_t Queue, uint16_t* OutSubJob);
bool				 JqQueueEmpty(uint8_t Queue);
bool				 JqPendingJobs(uint64_t Job);
void				 JqSelfPush(uint64_t Job, uint32_t JobIndex);
void				 JqSelfPop(uint64_t Job);
void				 JqFinishSubJob(uint16_t JobIndex);
void				 JqFinishInternal(uint16_t JobIndex);
void				 JqDecPrecondtion(uint64_t Handle, int Count);
void				 JqIncPrecondtion(uint64_t Handle, int Count);
void				 JqUnpackQueueLink(uint64_t Value, uint16_t& Head, uint16_t& Tail, uint16_t& JobCount);
uint64_t			 JqPackQueueLink(uint16_t Head, uint16_t Tail, uint16_t JobCount);
void				 JqUnpackStartAndQueue(uint64_t Value, uint16_t& PendingStart, uint8_t& Queue);
uint64_t			 JqPackStartAndQueue(uint16_t PendingStart, uint8_t Queue);
JqMutex&			 JqGetQueueMutex(uint64_t QueueIndex);
JqMutex&			 JqGetJobMutex(uint64_t JobIndex);
JqConditionVariable& JqGetJobConditionVariable(uint64_t Handle);
uint16_t			 JqDependentJobLinkAlloc(uint64_t Handle);
void				 JqDependentJobLinkFreeList(uint16_t Index);

// JqDependentJobLink&	 JqAllocDependentJobLink();
// void				 JqFreeDependentJobLink(JqDependentJobLink& Link);

struct JqSelfStack
{
	uint64_t Job;
	uint32_t JobIndex;
};

JQ_THREAD_LOCAL JqSelfStack JqSelfStack[JQ_MAX_JOB_STACK] = { { 0 } };
JQ_THREAD_LOCAL uint32_t	JqSelfPos					  = 0;
JQ_THREAD_LOCAL uint32_t	JqHasLock					  = 0;

// linked list structure, for when jobs have multiple jobs that depend on them
struct JqDependentJobLink
{
	uint64_t Job;
	uint16_t Next;
	uint64_t Owner;
};

struct JqJob
{
	JqFunction Function;

	std::atomic<uint64_t> StartedHandle;  /// Handle which has been added to the queue
	std::atomic<uint64_t> FinishedHandle; /// Handle which was last finished
	std::atomic<uint64_t> ClaimedHandle;  /// Handle which has claimed this header

	std::atomic<uint64_t> PendingFinish;		/// No. of jobs & (direct) child jobs that need to finish in order for this to be finished.
	std::atomic<uint64_t> PendingStartAndQueue; /// No. of Jobs that needs to be Started, and the queue which is it inserted to
	std::atomic<uint64_t> PreconditionCount;	/// No. of Preconditions that need to finish, before this can be enqueued.
	uint64_t			  Parent;				/// Handle of parent
	JqDependentJobLink	  DependentJob;			/// Job that is dependent on this job finishing.

	uint16_t NumJobs;	 /// Num Jobs to launch
	uint64_t Range;		 /// Range to pass to jobs
	uint32_t JobFlags;	 /// Job Flags
	uint8_t	 Queue;		 /// Priority of the job
	uint8_t	 Waiters;	 /// Set when waiting
	uint8_t	 WaitersWas; /// Prev wait flag(debug only)

	uint16_t Next;
	uint16_t Prev;

	bool Reserved;

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

#if JQ_LOCKLESS_POP

// Locked queue, from which we allow lockless inspection of emptiness + popping of jobs that -don't- change the head
struct JQ_ALIGN_CACHELINE JqQueue
{
	JqMutex				  Mutex;
	std::atomic<uint64_t> Link; // Head, Tail and job count.
};
#else

struct JqQueue
{
	JqMutex	 Mutex;
	uint16_t LinkHead;
	uint16_t LinkTail;
};

#endif

static_assert(sizeof(JqQueue) >= JQ_CACHE_LINE_SIZE, "sanity check for macros/alignment");

// note: This is just arbitrary: given a suffiently random priority setup you might need to bump this.

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore Semaphore[JQ_MAX_SEMAPHORES];

	// JqMutex				ConditionMutex;
	// JqConditionVariable WaitCond;

	uint64_t SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t	 QueueNumSemaphores[JQ_NUM_QUEUES];
	uint8_t	 QueueToSemaphore[JQ_NUM_QUEUES][JQ_MAX_SEMAPHORES];
	uint8_t	 SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t	 SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int		 ActiveSemaphores;
	// uint32_t m_JqInitFlags;
	JqAttributes Attributes;

	uint8_t		   NumQueues[JQ_MAX_THREADS];
	uint8_t		   QueueList[JQ_MAX_THREADS][JQ_NUM_QUEUES];
	uint8_t		   SemaphoreIndex[JQ_MAX_THREADS];
	JQ_THREAD	   WorkerThreads[JQ_MAX_THREADS];
	JqJobStackList StackSmall;
	JqJobStackList StackLarge;
	int			   NumWorkers;
	int			   Stop;
	int			   TotalWaiting;
	// uint32_t	   nFreeJobs;

	std::atomic<uint32_t> FreeJobs;
	std::atomic<uint64_t> NextHandle;

	JqQueueOrder		ThreadConfig[JQ_MAX_THREADS];
	JqJob				Jobs[JQ_JOB_BUFFER_SIZE];
	JqQueue				Queues[JQ_NUM_QUEUES];
	JqMutex				MutexJob[JQ_NUM_LOCKS];
	JqConditionVariable ConditionVariableJob[JQ_NUM_LOCKS];

	JqMutex				  DependentJobLinkMutex;
	JqDependentJobLink	  DependentJobLinks[JQ_JOB_BUFFER_SIZE];
	uint16_t			  DependentJobLinkHead;
	std::atomic<uint32_t> DependentJobLinkCounter;

	JqMutex WaitMutex;

	JqState_t()
		: NumWorkers(0)
	{
	}

	JqStats Stats;
} JqState;

#ifdef _WIN32
#pragma warning(pop)
#endif

JQ_THREAD_LOCAL int		 JqSpinloop				   = 0; // prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumQueues			   = 0;
JQ_THREAD_LOCAL uint8_t	 g_JqQueues[JQ_NUM_QUEUES] = { 0 };
JQ_THREAD_LOCAL JqJobStack* g_pJqJobStacks		   = 0;

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

	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & (uint64_t)&JqState) == 0);
	// JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.NumWorkers == 0);

	memset(JqState.QueueList, 0xff, sizeof(JqState.QueueList));
	memset(JqState.NumQueues, 0, sizeof(JqState.NumQueues));
	memset(JqState.SemaphoreMask, 0, sizeof(JqState.SemaphoreMask));
	memset(JqState.QueueNumSemaphores, 0, sizeof(JqState.QueueNumSemaphores));
	memset(JqState.QueueToSemaphore, 0, sizeof(JqState.QueueToSemaphore));
	memset(JqState.SemaphoreClients, 0, sizeof(JqState.SemaphoreClients));
	memset(JqState.SemaphoreClientCount, 0, sizeof(JqState.SemaphoreClientCount));
	JqState.ActiveSemaphores = 0;
	JqState.Attributes		 = *pAttr;
	JqState.NumWorkers		 = pAttr->NumWorkers;

	for(uint32_t i = 0; i < pAttr->NumWorkers; ++i)
	{
		JQ_ASSERT(pAttr->WorkerOrderIndex[i] < pAttr->NumQueueOrders); /// out of bounds pipe order index in attributes
		JQ_ASSERT(pAttr->WorkerOrderIndex[i] < JQ_NUM_QUEUES);
		JqState.ThreadConfig[i] = pAttr->QueueOrder[pAttr->WorkerOrderIndex[i]];
	}

	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JqQueueOrder& C				  = JqState.ThreadConfig[i];
		uint8_t		  nNumActivePipes = 0;
		uint64_t	  PipeMask		  = 0;
		static_assert(JQ_NUM_QUEUES < 64, "wont fit in 64bit mask");
		for(uint32_t j = 0; j < C.nNumPipes; ++j)
		{
			if(C.Queues[j] != 0xff)
			{
				JqState.QueueList[i][nNumActivePipes++] = C.Queues[j];
				JQ_ASSERT(C.Queues[j] < JQ_NUM_QUEUES);
				PipeMask |= 1llu << C.Queues[j];
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.NumQueues[i]   = nNumActivePipes;
		int nSelectedSemaphore = -1;
		for(int j = 0; j < JqState.ActiveSemaphores; ++j)
		{
			if(JqState.SemaphoreMask[j] == PipeMask)
			{
				nSelectedSemaphore = j;
				break;
			}
		}
		if(-1 == nSelectedSemaphore)
		{
			JQ_ASSERT(JqState.ActiveSemaphores < JQ_MAX_SEMAPHORES);
			nSelectedSemaphore						  = JqState.ActiveSemaphores++;
			JqState.SemaphoreMask[nSelectedSemaphore] = PipeMask;
			for(uint32_t j = 0; j < JQ_NUM_QUEUES; ++j)
			{
				if(PipeMask & (1llu << j))
				{
					JQ_ASSERT(JqState.QueueNumSemaphores[j] < JQ_MAX_SEMAPHORES);
					JqState.QueueToSemaphore[j][JqState.QueueNumSemaphores[j]++] = (uint8_t)nSelectedSemaphore;
				}
			}
		}
		JQ_ASSERT(JqState.SemaphoreClientCount[nSelectedSemaphore] < JQ_MAX_SEMAPHORES);
		JqState.SemaphoreClients[nSelectedSemaphore][JqState.SemaphoreClientCount[nSelectedSemaphore]++] = (uint8_t)i;
		JqState.SemaphoreIndex[i]																		 = (uint8_t)nSelectedSemaphore;
	}

	for(uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		JqState.Semaphore[i].Init(JqState.SemaphoreClientCount[i] ? JqState.SemaphoreClientCount[i] : 1);
	}

#if 0
	for (uint32_t i = 0; i < JQ_NUM_QUEUES; ++i)
	{
		printf("pipe %d : ", i);
		for (uint32_t j = 0; j < JqState.QueueNumSemaphores[i]; ++j)
		{
			printf("%d, ", JqState.QueueToSemaphore[i][j]);
		}
		printf("\n");
	}


	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		if (JqState.SemaphoreClientCount[i])
		{
			printf("Semaphore %d, clients %d Mask %llx :: ", i, JqState.SemaphoreClientCount[i], JqState.SemaphoreMask[i]);
			for (uint32_t j = 0; j < JqState.SemaphoreClientCount[i]; ++j)
			{
				printf("%d, ", JqState.SemaphoreClients[i][j]);
			}
			printf("\n");
		}
	}

	for (int i = 0; i < nNumWorkers; ++i)
	{
		printf("worker %d Sema %d Mask %llx :: Pipes ", i, JqState.SemaphoreIndex[i], JqState.SemaphoreMask[JqState.SemaphoreIndex[i]]);
		for (uint32_t j = 0; j < JqState.NumQueues[i]; ++j)
		{
			printf("%d, ", JqState.QueueList[i][j]);
		}
		printf("\n");

	}
#endif
	JqState.TotalWaiting = 0;
	JqState.Stop		 = 0;
	for(int i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		JqState.Jobs[i].StartedHandle  = 0;
		JqState.Jobs[i].FinishedHandle = 0;
	}
	// memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.FreeJobs			   = JQ_NUM_JOBS;
	JqState.NextHandle			   = 1;
	JqState.Stats.nNumFinished	   = 0;
	JqState.Stats.nNumLocks		   = 0;
	JqState.Stats.nNumSema		   = 0;
	JqState.Stats.nNumLocklessPops = 0;
	JqState.Stats.nNumWaitCond	   = 0;

	for(uint16_t i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		// terminate at end, and tag zero as unusable
		if(i == 0 || i == (JQ_JOB_BUFFER_SIZE - 1))
		{
			JqState.DependentJobLinks[i].Next = 0;
		}
		else
		{
			JqState.DependentJobLinks[i].Next = i + 1;
		}
		JqState.DependentJobLinks[i].Job = 0;
	}
	JqState.DependentJobLinkHead = 1;

	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JQ_THREAD_CREATE(&JqState.WorkerThreads[i]);
		JQ_THREAD_START(&JqState.WorkerThreads[i], JqWorker, (i));
	}
}
int JqNumWorkers()
{
	return JqState.NumWorkers;
}

void JqStop()
{
	JqWaitAll();
	JqState.Stop = 1;
	for(int i = 0; i < JqState.ActiveSemaphores; ++i)
	{
		JqState.Semaphore[i].Signal(JqState.NumWorkers);
	}
	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JQ_THREAD_JOIN(&JqState.WorkerThreads[i]);
		JQ_THREAD_DESTROY(&JqState.WorkerThreads[i]);
	}
	JqFreeAllStacks(JqState.StackSmall);
	JqFreeAllStacks(JqState.StackLarge);

	JqState.NumWorkers = 0;
}

void JqSetThreadQueueOrder(JqQueueOrder* pConfig)
{
	JQ_ASSERT(g_nJqNumQueues == 0); // its not supported to change this value, nor is it supported to set it on worker threads. set on init instead.
	uint32_t nNumActiveQueues = 0;
	JQ_ASSERT(pConfig->nNumPipes <= JQ_NUM_QUEUES);
	for(uint32_t j = 0; j < pConfig->nNumPipes; ++j)
	{
		if(pConfig->Queues[j] != 0xff)
		{
			g_JqQueues[nNumActiveQueues++] = pConfig->Queues[j];
			JQ_ASSERT(pConfig->Queues[j] < JQ_NUM_QUEUES);
		}
	}
	g_nJqNumQueues = nNumActiveQueues;
}

void JqConsumeStats(JqStats* pStats)
{
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
	JqState.Stats.nNextHandle	   = JqState.NextHandle;
}

void JqCheckFinished(uint64_t nJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint32_t nIndex = nJob % JQ_JOB_BUFFER_SIZE;
	// JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].FinishedHandle, nJob));
	// JQ_ASSERT(nJob == JqState.Jobs[nIndex].StartedHandle);

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
	// 	pEntry->FinishedHandle = pEntry->StartedHandle;
	// 	JQ_CLEAR_FUNCTION(pEntry->Function);

	// 	uint64_t Precondition	   = pEntry->nPreconditionFirst;
	// 	pEntry->nPreconditionFirst = 0;

	// 	while(Precondition != 0)
	// 	{
	// 		uint32_t nPreconditionIndex	   = Precondition % JQ_JOB_BUFFER_SIZE;
	// 		JqJob*	 pStalled			   = &JqState.Jobs[nPreconditionIndex];
	// 		pStalled->nStalled			   = 0;
	// 		Precondition				   = pStalled->nPreconditionSibling;
	// 		pStalled->nPreconditionSibling = 0;

	// 		JqPriorityListAdd(nPreconditionIndex);
	// 	}

	// 	JqState.Stats.nNumFinished++;
	// 	// kick waiting threads.
	// 	int8_t Waiters = pEntry->Waiters;
	// 	if(Waiters != 0)
	// 	{
	// 		JqState.Stats.nNumSema++;
	// 		JqState.WaitCond.NotifyAll();
	// 		pEntry->Waiters	= 0;
	// 		pEntry->WaitersWas = Waiters;
	// 	}
	// 	else
	// 	{
	// 		pEntry->WaitersWas = 0xff;
	// 	}
	// 	JqState.nFreeJobs++;
	// }
}

uint16_t JqIncrementStarted(uint64_t nJob)
{
	JQ_BREAK();
	// JQ_ASSERT_LOCKED(JqState.Mutex);
	// uint16_t nIndex = nJob % JQ_JOB_BUFFER_SIZE;
	// JQ_ASSERT(JqState.Jobs[nIndex].nNumJobs > JqState.Jobs[nIndex].nNumStarted);
	// uint16_t nSubIndex = JqState.Jobs[nIndex].nNumStarted++;
	// if(JqState.Jobs[nIndex].nNumStarted == JqState.Jobs[nIndex].nNumJobs)
	// {
	// 	JqPriorityListRemove(nIndex);
	// }
	// return nSubIndex;
}

void JqFinishInternal(uint16_t nJobIndex)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqFinishSubJob", 0xffff);

	nJobIndex	 = nJobIndex % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job = JqState.Jobs[nJobIndex];
	uint16_t PendingStart;
	uint8_t	 Queue;
	uint64_t PendingStartAndQueue = Job.PendingStartAndQueue.load();
	JqUnpackStartAndQueue(PendingStartAndQueue, PendingStart, Queue);
	JQ_ASSERT(PendingStart == 0);
	JQ_ASSERT(Job.PendingFinish.load() == 0);
	// no need to lock queue, since we are removed a long time ago

	uint16_t Parent = 0;
	// uint64_t DependentJob = 0;
	JqDependentJobLink Dependent;
	{
		JqSingleMutexLock L(JqGetJobMutex(nJobIndex));
		Parent = Job.Parent;
		JQ_CLEAR_FUNCTION(Job.Function);

		Dependent			  = Job.DependentJob;
		Job.DependentJob.Job  = 0;
		Job.DependentJob.Next = 0;
		// if(Job.DependentJob)
		// {
		// 	DependentJob	 = Job.DependentJob;
		// 	Job.DependentJob = 0;
		// }

		JqState.Stats.nNumFinished++;
		// kick waiting threads.
		int8_t Waiters = Job.Waiters;
		if(Waiters != 0)
		{
			JqState.Stats.nNumWaitKicks++;
			JqGetJobConditionVariable(nJobIndex).NotifyAll();
			Job.Waiters	   = 0;
			Job.WaitersWas = Waiters;
		}
		else
		{
			Job.WaitersWas = 0xff;
		}
		JqState.FreeJobs++;
		Job.FinishedHandle = Job.StartedHandle.load();
	}
	if(Dependent.Job)
	{
		JqDecPrecondtion(Dependent.Job, 1);
		uint16_t Next = Dependent.Next;
		while(Next)
		{
			uint64_t Job = JqState.DependentJobLinks[Next].Job;
			Next		 = JqState.DependentJobLinks[Next].Next;
			JqDecPrecondtion(Job, 1);
		}
	}
	if(Parent)
		JqFinishSubJob(Parent);
	if(Dependent.Next)
	{
		// Note: first element is embedded so it doesn't need freeing.
		JqDependentJobLinkFreeList(Dependent.Next);
	}
}

void JqFinishSubJob(uint16_t nJobIndex)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqFinishSubJob", 0xffff);
	nJobIndex		   = nJobIndex % JQ_JOB_BUFFER_SIZE;
	JqJob& Job		   = JqState.Jobs[nJobIndex];
	int	   FinishIndex = --Job.PendingFinish;
	JqState.Stats.nNumFinishedSub++;
	JQ_ASSERT(FinishIndex >= 0);
	if(0 == FinishIndex)
	{
		JqFinishInternal(nJobIndex);
	}
}

void JqAttachChild(uint64_t Parent, uint64_t Child)
{
	if(!Parent)
		return;
	uint16_t ParentIndex = Parent % JQ_JOB_BUFFER_SIZE;
	uint16_t ChildIndex	 = Child % JQ_JOB_BUFFER_SIZE;
	JqJob&	 ParentJob	 = JqState.Jobs[ParentIndex];
	JqJob&	 ChildJob	 = JqState.Jobs[ChildIndex];
	// can't add parent/child relations to already finished jobs.
	JQ_ASSERT(ParentJob.FinishedHandle != Parent);
	JQ_ASSERT(ChildJob.FinishedHandle != Child);
	// handles must be claimed
	JQ_ASSERT(ParentJob.ClaimedHandle == Parent);
	JQ_ASSERT(ChildJob.ClaimedHandle == Child);

	ParentJob.PendingFinish++;
	JQ_ASSERT(ChildJob.Parent == 0);
	ChildJob.Parent = Parent;
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

void JqSelfPush(uint64_t Job, uint32_t SubIndex)
{
	JqSelfStack[JqSelfPos].Job		= Job;
	JqSelfStack[JqSelfPos].JobIndex = SubIndex;
	JqSelfPos++;
}

void JqSelfPop(uint64_t Job)
{
	JQ_ASSERT(JqSelfPos != 0);
	JqSelfPos--;
	JQ_ASSERT(JqSelfStack[JqSelfPos].Job == Job);
}

uint32_t JqSelfJobIndex()
{
	JQ_ASSERT(JqSelfPos != 0);
	return JqSelfStack[JqSelfPos - 1].JobIndex;
}

int JqGetNumWorkers()
{
	return JqState.NumWorkers;
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
	return bSmall ? JqState.StackSmall : JqState.StackLarge;
}

void JqRunInternal(uint32_t nWorkIndex, int nBegin, int nEnd)
{
	JQ_ASSERT(JqState.Jobs[nWorkIndex].PreconditionCount == 0);

	if(JQ_INIT_USE_SEPERATE_STACK == (JqState.Attributes.Flags & JQ_INIT_USE_SEPERATE_STACK))
	{
		uint32_t	nFlags	   = JqState.Jobs[nWorkIndex].JobFlags;
		bool		bSmall	   = 0 != (nFlags & JQ_JOBFLAG_SMALL_STACK);
		uint32_t	nStackSize = bSmall ? JqState.Attributes.StackSizeSmall : JqState.Attributes.StackSizeLarge;
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

	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);

	JqSelfPush(nJob, nSubIndex);
	{
		uint16_t nWorkIndex = nJob % JQ_JOB_BUFFER_SIZE;
		uint16_t nNumJobs	= JqState.Jobs[nWorkIndex].NumJobs;
		int		 nRange		= JqState.Jobs[nWorkIndex].Range;
		int		 nFraction	= nRange / nNumJobs;
		int		 nRemainder = nRange - nFraction * nNumJobs;
		int		 nStart		= JqGetRangeStart(nSubIndex, nFraction, nRemainder);
		int		 nEnd		= JqGetRangeStart(nSubIndex + 1, nFraction, nRemainder);
		JqRunInternal(nWorkIndex, nStart, nEnd);
	}
	JqSelfPop(nJob);

	JqFinishSubJob(nJob);
}

uint16_t JqTakeJob(uint16_t* pSubIndex, uint32_t nNumQueues, uint8_t* pQueues)
{
	JQ_MICROPROFILE_SCOPE("JQ_TAKE_JOB", MP_AUTO); // if this starts happening the job queue size should be increased..
	const uint32_t nCount = nNumQueues ? nNumQueues : JQ_NUM_QUEUES;
	for(uint32_t i = 0; i < nCount; i++)
	{
		uint8_t	 nQueue = nNumQueues ? pQueues[i] : i;
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
// 			*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].StartedHandle);
// 			return nIndex;
// 		}
// 	}
// }
// else
// {
// 	for(int i = 0; i < JQ_NUM_QUEUES; ++i)
// 	{
// 		uint16_t nIndex = JqState.nPrioListHead[i];
// 		if(nIndex)
// 		{
// 			JQ_ASSERT(JqState.Jobs[nIndex].nStalled == 0);
// 			*pSubIndex = JqIncrementStarted(JqState.Jobs[nIndex].StartedHandle);
// 			return nIndex;
// 		}
// 	}
// }
// return 0;
// }

// #ifdef JQ_ASSERT_SANITY
// void JqTagChildren(uint16_t nRoot)
// {
// 	for(int i = 1; i < JQ_JOB_BUFFER_SIZE; ++i)
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
// 	for(int i = 1; i < JQ_JOB_BUFFER_SIZE; ++i)
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
	// uint16_t nRoot = nJob % JQ_JOB_BUFFER_SIZE;
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
	// 		for(int i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	// 		{
	// 			JqState.Jobs[i].nTag = 0;
	// 		}
	// 		JqTagChildren(nJob % JQ_JOB_BUFFER_SIZE);
	// 		uint16_t nIndex		= nJob % JQ_JOB_BUFFER_SIZE;
	// 		uint16_t nRootIndex = nJob % JQ_JOB_BUFFER_SIZE;
	// 		do
	// 		{
	// 			nIndex = JqTreeIterate(nJob, nIndex);
	// 			JQ_ASSERT(JqState.Jobs[nIndex].nTag == 1);
	// 			JqState.Jobs[nIndex].nTag = 0;
	// 		} while(nIndex != nRootIndex);
	// 		for(int i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	// 		{
	// 			JQ_ASSERT(JqState.Jobs[i].nTag == 0);
	// 		}
	// 	}
	// #endif

	// 	uint16_t nRoot	= nJob % JQ_JOB_BUFFER_SIZE;
	// 	uint16_t nIndex = nRoot;

	// 	do
	// 	{
	// 		nIndex = JqTreeIterate(nJob, nIndex);
	// 		JQ_ASSERT(nIndex);

	// 		if(JqState.Jobs[nIndex].nNumStarted < JqState.Jobs[nIndex].nNumJobs)
	// 		{
	// 			JQ_ASSERT(JqState.Jobs[nIndex].nStalled == 0);
	// 			*pSubIndexOut = JqIncrementStarted(JqState.Jobs[nIndex].StartedHandle);
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
	// JqIncrementFinished(nWork, JqState.Jobs[nWork].StartedHandle);
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.Jobs[nWork].StartedHandle);
	// }
	return true;
}

void JqWorker(int nThreadId)
{

	uint8_t* pQueues	= JqState.QueueList[nThreadId];
	uint32_t nNumQueues = JqState.NumQueues[nThreadId];
	g_nJqNumQueues		= nNumQueues; // even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqQueues, pQueues, nNumQueues);
	int nSemaphoreIndex = JqState.SemaphoreIndex[nThreadId];

#if MICROPROFILE_ENABLED
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker) - 1, "JqWorker %d %08llx", nThreadId, JqState.SemaphoreMask[nSemaphoreIndex]);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.Stop)
	{
		do
		{
			uint16_t nSubIndex = 0;
			uint16_t nWork	   = JqTakeJob(&nSubIndex, nNumQueues, pQueues);
			if(!nWork)
				break;
			JqExecuteJob(JqState.Jobs[nWork].StartedHandle, nSubIndex);
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
	if(0 == (nJob % JQ_JOB_BUFFER_SIZE))
	{
		nJob++;
	}
	return nJob;
}

// Claim a handle. reset header, and initialize its pending count to 1
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
				JqExecuteJob(JqState.Jobs[nIndex].StartedHandle, nSubIndex);
				Lock.Lock();
				JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
			}
		}
		else
		{
			JQ_BREAK(); // out of job queue space. increase JQ_JOB_BUFFER_SIZE or create fewer jobs
		}
	}
	JQ_ASSERT(JqState.nFreeJobs != 0);
	uint64_t nNextHandle = JqState.nNextHandle;
	uint16_t nCount		 = 0;
	while(JqPendingJobs(nNextHandle))
	{
		nNextHandle = JqNextHandle(nNextHandle);
		JQ_ASSERT(nCount++ < JQ_JOB_BUFFER_SIZE);
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

		uint16_t index = h % JQ_JOB_BUFFER_SIZE;
		if(index == 0) // slot 0 is reserved
			continue;
		pJob = &JqState.Jobs[index];

		uint64_t finished = pJob->FinishedHandle.load();
		uint64_t claimed  = pJob->ClaimedHandle.load();

		if(finished != claimed) // if the last claimed job is not yet done, we cant use it
			continue;

		if(pJob->ClaimedHandle.compare_exchange_strong(claimed, h))
			break;
	}
	// reset header
	pJob->PendingFinish = 0;
#if JQ_LOCKLESS_POP == 2
	pJob->PendingStartAndQueue = JqPackStartAndQueue(0, 0xff);
#else
	pJob->PendingStartAndQueue = JqPackStartAndQueue(0, 0);
#endif
	pJob->PreconditionCount = 1;
	pJob->Parent			= 0;
	pJob->DependentJob.Job	= 0;
	pJob->DependentJob.Next = 0;
	//	pJob->Locked			= false;
	pJob->NumJobs  = 0;
	pJob->Range	   = 0;
	pJob->JobFlags = JQ_JOBFLAG_UNINITIALIZED;
	pJob->Waiters  = 0;
	pJob->Next	   = 0;
	pJob->Prev	   = 0;
	pJob->Reserved = false;

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
	// uint16_t	nIndex = nJob % JQ_JOB_BUFFER_SIZE;

	// JqJob* pEntry = &JqState.Jobs[nIndex];
	// if(pEntry->StartedHandle != nJob)
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
	uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
	JqJob*	 pJob  = &JqState.Jobs[Index];

	uint64_t Before = pJob->PreconditionCount.fetch_add(Count);
	// Adding preconditions is only allowed when the job has been reserved, in which case it'll have a count of 1 or larger
	// If we don't do it this way, jobs may be picked up before having their preconditions added.
	JQ_ASSERT(Before > 0);
}

void JqDecPrecondtion(uint64_t Handle, int Count)
{
	uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job   = JqState.Jobs[Index];
	JQ_ASSERT(Job.ClaimedHandle == Handle);
	JQ_ASSERT(JQ_LT_WRAP(Job.StartedHandle, Handle));
	JQ_ASSERT(JQ_LT_WRAP(Job.FinishedHandle, Handle));

	uint64_t Before = Job.PreconditionCount.fetch_add(-Count);

	if(Before - Count == 0)
	{
		// only place StartedHandle is modified
		uint32_t NumJobs  = Job.NumJobs;
		Job.StartedHandle = Handle;
		if(NumJobs == 0) // Barrier type Jobs never have to enter an actual queue.
		{
			JqFinishInternal(Index);
		}
		else
		{
			uint8_t Queue = Job.Queue;
			JqQueuePush(Queue, Handle);
			{
				if(0)
				{
					// JQ_MAX_SEMAPHORES
					for(uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
					{
						// int nSemaIndex = JqState.QueueToSemaphore[Queue][i];
						JqState.Semaphore[i].Signal(-1);
					}
				}
				else
				{
					uint32_t nNumSema = JqState.QueueNumSemaphores[Queue];
					for(uint32_t i = 0; i < nNumSema; ++i)
					{
						int nSemaIndex = JqState.QueueToSemaphore[Queue][i];
						JqState.Semaphore[nSemaIndex].Signal(NumJobs);
					}
				}
			}
		}
	}
}

JqMutex& JqGetQueueMutex(uint64_t Queue)
{
	return JqState.Queues[Queue % JQ_NUM_QUEUES].Mutex;
}

JqMutex& JqGetJobMutex(uint64_t Handle)
{
	return JqState.MutexJob[Handle % JQ_NUM_LOCKS];
}

JqConditionVariable& JqGetJobConditionVariable(uint64_t Handle)
{
	return JqState.ConditionVariableJob[Handle % JQ_NUM_LOCKS];
}

inline void JqUnpackQueueLink(uint64_t Value, uint16_t& Head, uint16_t& Tail, uint16_t& JobCount)
{
	Head	 = Value & 0xffff;
	Tail	 = (Value >> 16) & 0xffff;
	JobCount = (Value >> 32) & 0xffff;
}

inline uint64_t JqPackQueueLink(uint16_t Head, uint16_t Tail, uint16_t JobCount)
{
	uint64_t Value = Head | ((uint64_t)Tail << 16) | ((uint64_t)JobCount << 32);
	return Value;
}

inline void JqUnpackStartAndQueue(uint64_t Value, uint16_t& PendingStart, uint8_t& Queue)
{
	PendingStart = Value & 0xffff;
	Queue		 = (Value >> 16) & 0xff;
}

inline uint64_t JqPackStartAndQueue(uint16_t PendingStart, uint8_t Queue)
{
	uint64_t Value = PendingStart | (Queue << 16);
	return Value;
}

void JqQueuePush(uint8_t QueueIndex, uint64_t Handle)
{
	uint16_t JobIndex = Handle % JQ_JOB_BUFFER_SIZE;
	// JQ_ASSERT(JobIndex < JQ_JOB_BUFFER_SIZE);
	JQ_ASSERT(QueueIndex < JQ_NUM_QUEUES);
	JqJob&	 Job   = JqState.Jobs[JobIndex];
	JqQueue& Queue = JqState.Queues[QueueIndex];

	JQ_ASSERT(Job.PreconditionCount == 0);
	JQ_ASSERT(Job.Queue == QueueIndex);

	uint64_t Started  = Job.StartedHandle;
	uint64_t Finished = Job.FinishedHandle;
	uint64_t Claimed  = Job.ClaimedHandle;
	JQ_ASSERT(Started == Claimed);
	JQ_ASSERT(JQ_LT_WRAP(Finished, Claimed));
	JQ_ASSERT(Job.NumJobs);

	uint16_t NumJobs = Job.NumJobs;
	JQ_ASSERT(NumJobs < 0xffff);

	Job.PendingFinish = NumJobs;

	JqSingleMutexLock L(Queue.Mutex);

#if !JQ_LOCKLESS_POP

	Job.PendingStartAndQueue = JqPackStartAndQueue(NumJobs, 0);

	Job.Prev = Queue.LinkTail;
	if(Queue.LinkTail)
	{
		JQ_ASSERT(JobIndex);
		JqState.Jobs[Queue.LinkTail].Next = JobIndex;
		Queue.LinkTail					  = JobIndex;
	}
	else
	{
		JQ_ASSERT(Queue.LinkHead == 0);
		Queue.LinkHead = Queue.LinkTail = JobIndex;
	}
#elif JQ_LOCKLESS_POP == 2

	Job.PendingStartAndQueue = JqPackStartAndQueue(NumJobs, QueueIndex);

	uint16_t Head, Tail, JobCount;

	JQ_ASSERT(Job.Prev == 0);
	JQ_ASSERT(Job.Next == 0);

	uint64_t Old = Queue.Link.load();
	JqUnpackQueueLink(Old, Head, Tail, JobCount);
	if(Head)
		JQ_ASSERT(JobCount == 0);
	else
		JQ_ASSERT(!Tail);

	Job.Prev = Tail;
	if(Tail)
	{
		JQ_ASSERT(JobIndex);
		JqState.Jobs[Tail].Next = JobIndex;
		Tail					= JobIndex;
	}
	else
	{
		JQ_ASSERT(Head == 0);
		Tail	 = JobIndex;
		Head	 = JobIndex;
		JobCount = 0;
	}
	Queue.Link = JqPackQueueLink(Head, Tail, JobCount);
	// printf("pushed %d(%d) -> %d ... %d/%d\n", Handle, JobCount, QueueIndex, Head, Tail);

#else

	// Note: We have to use atomics, as lockless decrements of job count will occur. Tail/Head will not be modified without taking the lock
	uint16_t Head, Tail, JobCount;
	uint64_t Old, New;
	Job.PendingStartAndQueue = JqPackStartAndQueue(NumJobs, 0);

	do
	{
		Old = Queue.Link.load();
		JqUnpackQueueLink(Old, Head, Tail, JobCount);
		if(Head)
			JQ_ASSERT(JobCount > 0);
		else
			JQ_ASSERT(!Tail);

		Job.Prev = Tail;
		if(Tail)
		{
			JQ_ASSERT(JobIndex);
			JqState.Jobs[Tail].Next = JobIndex;
			Tail					= JobIndex;
		}
		else
		{
			JQ_ASSERT(Head == 0);
			Tail	 = JobIndex;
			Head	 = JobIndex;
			JobCount = NumJobs;
		}

		JQ_ASSERT(JobCount);

		New = JqPackQueueLink(Head, Tail, JobCount);
	} while(!Queue.Link.compare_exchange_weak(Old, New));

#endif
}

// Tries to take a job at JobIndex, iff its still in Queue QueueIndex
//  - If its the last job, it will lock the queue and remove the Job from the queue
//  - if its not the last, it will -not- lock
//  - if all jobs are popped, and it has yet to be removed from the queue, it'll return its next in OutNextHandle, to allow peeking on
uint16_t JqQueuePopInternal(uint16_t JobIndex, uint8_t QueueIndex, uint16_t* OutSubJob, uint16_t* OutNextJob, uint16_t* OutMustRetry, bool PopAll)
{
#if 0 == JQ_LOCKLESS_POP
	JQ_BREAK();
	return 0;
#else
	JobIndex		 = JobIndex % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job	 = JqState.Jobs[JobIndex];
	uint16_t NumJobs = Job.NumJobs;
	uint8_t	 JobQueue;
	uint16_t JobSubIndex;
	uint64_t Old, New;
	uint16_t PopCount;
	do
	{
		JobQueue	= 0xff;
		JobSubIndex = 0xffff;
		PopCount	= 0;
		Old			= Job.PendingStartAndQueue.load();
		JqUnpackStartAndQueue(Old, JobSubIndex, JobQueue);
		if(JobQueue != QueueIndex)
		{

			return 0; // Job at JobIndex is in a different queue, so bail
		}

		if(JobSubIndex == 0) // last element has been popped, so we won't be able to pop any job
		{
			// return the next pointer. caller can use this to peek for more work.
			// Note that this is not strictly synchronized, but peeking checks that it is indeed from the correct queue
			*OutNextJob	  = Job.Next;
			*OutMustRetry = 1;

			// note: what happens here when someone else inserts

			return 0;
		}
		if(PopAll)
		{
			PopCount	= JobSubIndex;
			JobSubIndex = 0;
		}
		else
		{
			PopCount = 1;
			JobSubIndex--;
		}
		JQ_ASSERT(JobSubIndex != 0xffff);
		New = JqPackStartAndQueue(JobSubIndex, JobSubIndex == 0 ? 0xff : JobQueue);
	} while(!Job.PendingStartAndQueue.compare_exchange_weak(Old, New));

	// thread hitting zero is resposible for locking and removing from the queue
	if(JobSubIndex == 0)
	{
		JqQueue& Q = JqState.Queues[QueueIndex];
		JQ_MICROPROFILE_SCOPE("LockedPop", MP_AUTO);

		JqSingleMutexLock L(Q.Mutex);
		{
			uint16_t Head, Tail, JobCount;
			uint64_t Old = Q.Link;
			JqUnpackQueueLink(Old, Head, Tail, JobCount);
			// printf("Pop %d -> %d ... %d/%d\n", JobIndex, QueueIndex, Head, Tail);

			uint16_t NewHead = Head;
			uint16_t NewTail = Tail;
			JQ_ASSERT(JobCount == 0);
			if(Head == Tail)
			{
				JQ_ASSERT(Head == JobIndex);
				NewHead = NewTail = 0;
			}
			else if(Head == JobIndex)
			{
				JQ_ASSERT(Job.Prev == 0);
				JQ_ASSERT(Tail != JobIndex);
				NewHead = Job.Next;
				JQ_ASSERT(JqState.Jobs[NewHead].Prev == JobIndex);
				JqState.Jobs[NewHead].Prev = 0;
			}
			else if(Tail == JobIndex)
			{
				JQ_ASSERT(Job.Next == 0);
				JQ_ASSERT(Head != JobIndex);
				NewTail = Job.Prev;
				JQ_ASSERT(JqState.Jobs[NewTail].Next == JobIndex);
				JqState.Jobs[NewTail].Next = 0;
			}
			else
			{
				JQ_ASSERT(JobIndex != Tail);
				JQ_ASSERT(JobIndex != Head);
				JQ_ASSERT(Job.Next);
				JQ_ASSERT(Job.Prev);

				JqJob& PrevJob = JqState.Jobs[Job.Prev];
				JqJob& NextJob = JqState.Jobs[Job.Next];
				JQ_ASSERT(PrevJob.Next == JobIndex);
				JQ_ASSERT(NextJob.Prev == JobIndex);
				PrevJob.Next = Job.Next;
				NextJob.Prev = Job.Prev;
			}
			Q.Link	 = JqPackQueueLink(NewHead, NewTail, 0);
			Job.Next = 0;
			Job.Prev = 0;
		}
	}
	if(PopAll)
	{
		*OutSubJob = 0;
	}
	else
	{
		*OutSubJob = NumJobs - 1 - JobSubIndex;
	}

	return PopCount;
#endif
}

bool JqQueueEmpty(uint8_t QueueIndex)
{
	JQ_ASSERT(QueueIndex < JQ_NUM_QUEUES);
	JqQueue& Queue = JqState.Queues[QueueIndex];
#if JQ_LOCKLESS_POP
	uint64_t Link = Queue.Link.load();
	uint16_t Head, Tail, JobCount;
	JqUnpackQueueLink(Link, Head, Tail, JobCount);
	return Head == Tail && Head == 0;
#else
	JqSingleMutexLock L(Queue.Mutex);
	return Queue.LinkHead == Queue.LinkTail && Queue.LinkHead == 0;
#endif
}

uint16_t JqQueuePop(uint8_t QueueIndex, uint16_t* OutSubJob)
{
	JQ_ASSERT(QueueIndex < JQ_NUM_QUEUES);
	JqQueue& Queue = JqState.Queues[QueueIndex];
	JQ_MICROPROFILE_SCOPE("Pop", MP_AUTO);
#if 2 == JQ_LOCKLESS_POP

	uint16_t PopCount  = 0;
	uint16_t JobIndex  = 0;
	uint16_t SubIndex  = 0xffff;
	uint16_t MustRetry = 0;
	do
	{
		PopCount  = 0;
		JobIndex  = 0;
		MustRetry = 0;
		SubIndex  = 0xffff;

		uint16_t PeekNext = 0;
		uint16_t Head, Tail, JobCount;

		uint64_t Old, New;
		(void)New;

		Old = Queue.Link.load();
		JqUnpackQueueLink(Old, Head, Tail, JobCount);
#if JQ_LOCKLESS_PEEK_COUNT

		uint16_t PopLocation = Head;

		for(int i = 0; i < JQ_LOCKLESS_PEEK_COUNT + 1; ++i)
		{
			if(!PopLocation)
				break;
			if(0 != (PopCount = JqQueuePopInternal(PopLocation, QueueIndex, &SubIndex, &PeekNext, false)))
			{
				JobIndex = PopLocation;
				break;
			}
			else
			{
				PopLocation = PeekNext;
			}
		}
		if(JobIndex)
		{
			break;
		}
#else

		if(!Head) // Nothing to pop
			break;
		if(0 != (PopCount = JqQueuePopInternal(Head, QueueIndex, &SubIndex, &PeekNext, &MustRetry, false)))
		{
			JobIndex = Head;
			JQ_ASSERT(JobIndex);
			break;
		}
		else
		{
			// if(!MustRetry)
			// {
			// 	break;
			// }
		}

#endif
	} while(true);

	if(JobIndex)
	{
		*OutSubJob = SubIndex;
	}
	if(!JobIndex)
	{
		// if(!JqQueueEmpty(QueueIndex))
		// {
		// 	JQ_BREAK();
		// }
	}
	return JobIndex;

#elif 1 == JQ_LOCKLESS_POP
	uint16_t JobIndex;
	int		 SubIndex;

	uint16_t Head, Tail, JobCount;
	uint64_t Old, New;
	do
	{
		JobIndex = 0;
		SubIndex = -1;
		Old		 = Queue.Link.load();
		JqUnpackQueueLink(Old, Head, Tail, JobCount);
		if(!Head)
			break; // empty
		if(JobCount < 2)
		{
			JQ_ASSERT(JobCount == 1);
			// we'll be hitting 0, so we have to lock
			JQ_MICROPROFILE_SCOPE("LockedPop", MP_AUTO);
			JqSingleMutexLock L(Queue.Mutex);

			Old = Queue.Link.load();
			JqUnpackQueueLink(Old, Head, Tail, JobCount);
			if(!Head)
				break; // empty

			if(JobCount == 1) // only update if its still 1. someone else might've gotten the lock first, in which case we fall down to the lockless pop.
			{
				uint16_t NewHead;
				uint16_t NewTail = Tail;
				uint16_t NewCount;

				JqJob& Job = JqState.Jobs[Head];
				if(Head == Tail) // last element in queue
				{
					JQ_ASSERT(Job.Next == 0);
					NewHead	 = 0;
					NewTail	 = 0;
					NewCount = 0;
				}
				else
				{
					JQ_ASSERT(Job.Next);

					NewHead		   = Job.Next;
					JqJob& NextJob = JqState.Jobs[NewHead];
					JQ_ASSERT(NextJob.Prev == Head);
					NextJob.Prev = 0;

					NewCount = NextJob.NumJobs;

					// JQ_ASSERT(NewCount == NextJob.PendingStart.load());
				}
				New = JqPackQueueLink(NewHead, NewTail, NewCount);

				if(NewHead)
				{
					JQ_ASSERT(NewCount > 0);
				}
				else
				{
					JQ_ASSERT(NewTail == 0);
				}

				Job.Next = 0;
				JQ_ASSERT(Job.Prev == 0);

				bool Success = Queue.Link.compare_exchange_strong(Old, New);

				JQ_ASSERT(Success);

				SubIndex = 0;
				JobIndex = Head;
				break;
			}
		}
		{
			SubIndex = --JobCount;
			JobIndex = Head;
			JQ_ASSERT(JobCount > 0);
			New = JqPackQueueLink(Head, Tail, JobCount);
		}

	} while(!Queue.Link.compare_exchange_weak(Old, New));

	if(JobIndex)
	{
		JqJob& Job = JqState.Jobs[JobIndex];
		/// note: queue part is unused here.
		// JqUnpackStartAndQueue
		int UnusedSubIndex = --Job.PendingStartAndQueue; // not used anymore, kept directly with head.
		(void)UnusedSubIndex;
		JQ_ASSERT(UnusedSubIndex >= 0 && UnusedSubIndex < Job.NumJobs);

		*OutSubJob = Job.NumJobs - 1 - SubIndex; // do it in order.
	}

#else
	// JqQueue&		  Queue = JqState.Queues[QueueIndex];
	JqSingleMutexLock L(Queue.Mutex);

	uint16_t JobIndex = Queue.LinkHead;
	if(JobIndex)
	{
		JQ_ASSERT(JobIndex < JQ_JOB_BUFFER_SIZE);
		JqJob& Job		= JqState.Jobs[JobIndex];
		int	   SubIndex = --Job.PendingStartAndQueue;
		// printf("JqQueuePop job %d -> %d sub %d\n", JobIndex, QueueIndex, SubIndex);

		JQ_ASSERT(SubIndex >= 0); // should never go negative.
		if(SubIndex == 0)
		{
			Queue.LinkHead = Job.Next;
			if(JobIndex == Queue.LinkTail)
			{
				JQ_ASSERT(Queue.LinkHead == 0);
				Queue.LinkTail = 0;
			}
			if(Queue.LinkHead == 0)
				JQ_ASSERT(Queue.LinkTail == 0);
			if(Queue.LinkTail == 0)
				JQ_ASSERT(Queue.LinkHead == 0);
		}
		*OutSubJob = Job.NumJobs - 1 - SubIndex; // do it in order.
	}

#endif

	return JobIndex;
}

uint16_t JqDependentJobLinkAlloc(uint64_t Owner)
{
	JqSingleMutexLock L(JqState.DependentJobLinkMutex);

	JqState.DependentJobLinkCounter++;

	uint16_t Link = JqState.DependentJobLinkHead;
	// printf("** DEP JOB ALLOCATE   %5d :: %d :: %5d\n", Link, JqState.DependentJobLinkCounter.load(), Owner);
	if(!Link)
	{
		// If you're hitting this, then you're adding more than JQ_JOB_BUFFER_SIZE, on top of the one link thats already space for
		// you'd probably want to make this allocator lockless and grow it significantly
		JQ_BREAK();
	}
	JqState.DependentJobLinkHead		  = JqState.DependentJobLinks[Link].Next;
	JqState.DependentJobLinks[Link].Next  = 0;
	JqState.DependentJobLinks[Link].Job	  = 0;
	JqState.DependentJobLinks[Link].Owner = 0;
	return Link;
}

void JqDependentJobLinkFreeList(uint16_t Link)
{
	JqSingleMutexLock L(JqState.DependentJobLinkMutex);
	uint16_t		  Head = JqState.DependentJobLinkHead;
	while(Link)
	{
		JQ_ASSERT(Link < JQ_JOB_BUFFER_SIZE);
		JqState.DependentJobLinkCounter--;
		// printf("** DEP JOB FREE       %5d :: %d :: %5d\n", Link, JqState.DependentJobLinkCounter.load(), JqState.DependentJobLinks[Link].Owner);

		uint16_t Next = JqState.DependentJobLinks[Link].Next;

		JqState.DependentJobLinks[Link].Next  = Head;
		JqState.DependentJobLinks[Link].Owner = 0;
		JqState.DependentJobLinks[Link].Job	  = 0;
		Head								  = Link;

		Link = Next;
	}
	JqState.DependentJobLinkHead = Head;
}

void JqAddPreconditionInternal(uint64_t Handle, uint64_t Precondition)
{
	// only add if its actually not done.
	if(!JqIsDone(Precondition))
	{
		uint16_t nIndex		   = Handle % JQ_JOB_BUFFER_SIZE;
		uint16_t nPrecondIndex = Precondition % JQ_JOB_BUFFER_SIZE;
		JqJob&	 Job		   = JqState.Jobs[nIndex];
		JqJob&	 PrecondJob	   = JqState.Jobs[nPrecondIndex];
		JQ_ASSERT(Job.PreconditionCount > 0); // as soon as the existing precond count reaches 0, it might be executed, so it no longer makes sense to add new preconditions
											  // use mechanisms like JqReserve, to block untill precondtions have been added.
		JqIncPrecondtion(Handle, 1);
		bool	 Finished;
		uint16_t LinkIndex = 0;
		while(true)
		{
			Finished = false;
			JQ_ASSERT(!LinkIndex); // only

			if(PrecondJob.DependentJob.Job)
			{
				LinkIndex = JqDependentJobLinkAlloc(Precondition);
			}

			JqSingleMutexLock L(JqGetJobMutex(Precondition));
			if(PrecondJob.FinishedHandle == Precondition)
			{
				Finished = true;
			}
			else
			{
				if(PrecondJob.DependentJob.Job == 0)
				{
					JQ_ASSERT(PrecondJob.DependentJob.Next == 0);
					PrecondJob.DependentJob.Job = Handle;
				}
				else
				{
					if(!LinkIndex && PrecondJob.DependentJob.Job) // Note: Can't (wont!) lock two mutexes, so in this exceptional condition we retry
						continue;
					JqDependentJobLink& L		 = JqState.DependentJobLinks[LinkIndex];
					L.Job						 = Handle;
					L.Next						 = PrecondJob.DependentJob.Next;
					PrecondJob.DependentJob.Next = LinkIndex;
					LinkIndex					 = 0; // clear to indicate it was successfully inserted
				}
			}
			break;
		}
		if(LinkIndex)
		{
			JqDependentJobLinkFreeList(LinkIndex);
		}
		if(Finished) // the precondition job finished after we took the lock, so decrement manually.
		{
			JqDecPrecondtion(Handle, 1);
		}
	}
}

void JqAddPrecondition(uint64_t Handle, uint64_t Precondition)
{
	if(!JqState.Jobs[Handle % JQ_JOB_BUFFER_SIZE].Reserved)
	{
		JQ_BREAK();
		// you can only add precondtions to jobs that have been Reserved and not yet added/closed
		// you must do
		// h = JqReserve
		//  -> Call JqAddPrecondition  here
		// JqCloseReserved(h)/JqAddReserved
	}
	JqAddPreconditionInternal(Handle, Precondition);
}

uint64_t JqAddInternal(uint64_t ReservedHandle, JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags, uint64_t Precondition)
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

	uint64_t Parent = 0 != (JobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf();
	JQ_ASSERT(JqState.NumWorkers);
	JQ_ASSERT(NumJobs);

	if(Range < 0)
	{
		Range = NumJobs;
	}
	if(NumJobs < 0)
	{
		NumJobs = JqState.NumWorkers;
	}
	if(NumJobs >= 0xffff)
	{
		// Not supported right now.
		JQ_BREAK();
	}

	uint64_t Handle = 0;
	{
		if(ReservedHandle)
		{
			JQ_ASSERT(Queue == 0xff);
			Handle = ReservedHandle;
		}
		else
		{
			Handle = JqClaimHandle();
		}

		uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
		JqJob&	 Job   = JqState.Jobs[Index];

		if(ReservedHandle)
		{
			JQ_ASSERT(Job.ClaimedHandle == ReservedHandle);
			JQ_ASSERT(Job.Queue != 0xff);
			JQ_ASSERT(Job.PreconditionCount >= 1);
			if(!Job.Reserved)
			{
				JQ_BREAK(); // When reserving Job handles, you should call -either- JqCloseReserved or JqAddReserved, never both.
			}
			Job.Reserved = false;
		}
		else
		{
			JQ_ASSERT(Job.PreconditionCount == 1);
			JQ_ASSERT(Queue < JQ_NUM_QUEUES);
			Job.Queue	= Queue;
			Job.Waiters = 0;
		}

		JQ_ASSERT(Job.ClaimedHandle.load() == Handle);
		JQ_ASSERT(JQ_LT_WRAP(Job.FinishedHandle, Handle));

		JQ_ASSERT(NumJobs <= 0xffff);

		Job.NumJobs = NumJobs;
#if JQ_LOCKLESS_POP == 2
		Job.PendingStartAndQueue = JqPackStartAndQueue(0, 0xff);
#else
		Job.PendingStartAndQueue = JqPackStartAndQueue(0, 0);
#endif
		Job.PendingFinish = 0;

		// Job.PreconditionCount++;
		Job.Range	 = Range;
		Job.JobFlags = JobFlags;

		JqAttachChild(Parent, Handle);
		JQ_ASSERT(Job.Parent == Parent);

		Job.Function = JobFunc;

		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += NumJobs;
		if(Precondition)
		{
			JqAddPreconditionInternal(Handle, Precondition);
		}

		// Decrementing preconditions automatically add to a queue when reaching 0
		JQ_ASSERT(Job.PreconditionCount > 0);
		JqDecPrecondtion(Handle, 1);
	}
	return Handle;
}

uint64_t JqAdd(JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(0, JobFunc, Queue, NumJobs, Range, JobFlags, 0);
}

// add reserved
uint64_t JqAddReserved(uint64_t ReservedHandle, JqFunction JobFunc, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(ReservedHandle, JobFunc, 0xff, NumJobs, Range, JobFlags, 0);
}

// add successor
uint64_t JqAddSuccessor(uint64_t Precondition, JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(0, JobFunc, Queue, NumJobs, Range, JobFlags, Precondition);
}

// Reserve a Job slot. this allows you to wait on work added later
uint64_t JqReserve(uint8_t Queue, uint32_t JobFlags)
{
	JQ_ASSERT(Queue < JQ_NUM_QUEUES);

	uint64_t Handle = JqClaimHandle();
	uint16_t Index	= Handle % JQ_JOB_BUFFER_SIZE;

	JqJob& Job = JqState.Jobs[Index];

	JQ_ASSERT(JQ_LE_WRAP(Job.FinishedHandle, Handle));
	JQ_ASSERT(JQ_LE_WRAP(Job.StartedHandle, Handle));
	JQ_ASSERT(Job.ClaimedHandle == Handle);
	JQ_ASSERT(Job.PreconditionCount == 1);
	JQ_ASSERT(Job.NumJobs == 0);

	uint64_t Parent = 0 != (JobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf();
	Job.Reserved	= 1;

	if(Parent)
	{
		JqAttachChild(Parent, Handle);
	}

	JQ_CLEAR_FUNCTION(Job.Function);
	Job.Queue	= Queue;
	Job.Waiters = 0;

	return Handle;
}
// Mark reservation as finished, without actually executing any jobs.
void JqCloseReserved(uint64_t Handle)
{
	if(!JqState.Jobs[Handle % JQ_JOB_BUFFER_SIZE].Reserved)
	{
		JQ_BREAK(); // When reserving Job handles, you should call -either- JqCloseReserved or JqAddReserved, never both.
	}
	JqState.Jobs[Handle % JQ_JOB_BUFFER_SIZE].Reserved = false;
	JqDecPrecondtion(Handle, 1);
}

void JqDump()
{
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t Index			= nJob % JQ_JOB_BUFFER_SIZE;
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
		JQ_BREAK(); // todo, implement support for counting child jobs seperately.
					// uint64_t nIndex = nJob % JQ_JOB_BUFFER_SIZE;
					// return JqState.Jobs[nIndex].nNumJobs == JqState.Jobs[nIndex].NumFinished;
	}
	return bIsDone;
}

bool JqPendingJobs(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_JOB_BUFFER_SIZE;
	JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].FinishedHandle, JqState.Jobs[nIndex].StartedHandle));
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
	// 			JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
	// 		}
	// 		nIndex = JqTakeJob(&nSubIndex, 0, nullptr);
	// 	}
	// 	if(nIndex)
	// 	{
	// 		JqExecuteJob(JqState.Jobs[nIndex].StartedHandle, nSubIndex);
	// 	}
	// 	else
	// 	{
	// 		JQ_USLEEP(1000);
	// 	}
	// }
	// if(nIndex)
	// {
	// 	JqMutexLock lock(JqState.Mutex);
	// 	JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
	// }
}

void JqWait(uint64_t Job, uint32_t WaitFlag, uint32_t UsWaitTime)
{
	if(JqIsDone(Job))
	{
		return;
	}
	while(!JqIsDoneExt(Job, WaitFlag))
	{

		uint16_t SubIndex = 0;
		uint16_t Work	  = 0;
		if((WaitFlag & JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS) == JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS)
		{
			JQ_BREAK();
			// todo implement.
			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
			// if(JqIsDoneExt(Job, WaitFlag))
			// 	return;
			// nIndex = JqTakeChildJob(Job, &nSubIndex);
			// if(!nIndex)
			// {
			// 	nIndex = JqTakeJob(&nSubIndex, g_nJqNumQueues, g_JqQueues);
			// }
		}
		else if(WaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
			// if(JqIsDoneExt(Job, WaitFlag))
			// 	return;
			// nIndex = JqTakeChildJob(Job, &nSubIndex);
		}
		else if(0 != (WaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			SubIndex = 0;
			Work	 = JqTakeJob(&SubIndex, g_nJqNumQueues, g_JqQueues);

			// JqMutexLock lock(JqState.Mutex);
			// if(nIndex)
			// 	JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);
			// if(JqIsDoneExt(nJob, WaitFlag))
			// 	return;
			// nIndex = JqTakeJob(&nSubIndex, g_nJqNumQueues, g_JqQueues);
		}
		else
		{
			JQ_BREAK();
		}

		if(Work)
		{
			JqExecuteJob(JqState.Jobs[Work].StartedHandle, SubIndex);
		}
		else
		{
			JQ_ASSERT(0 != (WaitFlag & (JQ_WAITFLAG_SLEEP | JQ_WAITFLAG_BLOCK | JQ_WAITFLAG_SPIN)));
			if(WaitFlag & JQ_WAITFLAG_SPIN)
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
				} while((1000000ull * (JqTick() - nTick)) / nTicksPerSecond < UsWaitTime);
			}
			else if(WaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(UsWaitTime);
			}
			else
			{
				uint16_t		  nJobIndex = Job % JQ_JOB_BUFFER_SIZE;
				JqSingleMutexLock lock(JqGetJobMutex(nJobIndex));
				if(JqIsDoneExt(Job, WaitFlag))
				{
					return;
				}
				JqState.Jobs[nJobIndex].Waiters++;
				JqState.Stats.nNumWaitCond++;
				JqGetJobConditionVariable(nJobIndex).Wait(JqState.WaitMutex);
				JqState.Jobs[nJobIndex].Waiters--;
			}
		}
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
				JqIncrementFinished(JqState.Jobs[nIndex].StartedHandle);

			if(JqIsDone(nJob))
				return;

			nIndex = JqTakeChildJob(nJob, &nSubIndex);
		}
		if(nIndex)
		{
			JqExecuteJob(JqState.Jobs[nIndex].StartedHandle, nSubIndex);
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
	uint64_t Handle = JqClaimHandle();
	uint16_t Index	= Handle % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job	= JqState.Jobs[Index];

	JQ_ASSERT(JQ_LE_WRAP(Job.FinishedHandle, Handle));

	uint64_t Parent = JqSelf();
	if(Parent)
	{
		JqAttachChild(Parent, Handle);
	}
	JQ_ASSERT(Job.Parent == Parent);
	JQ_CLEAR_FUNCTION(Job.Function);
	Job.Queue = 0xff;
	JqSelfPush(Handle, 0);
	return Handle;
}

void JqGroupEnd()
{
	uint64_t Job = JqSelf();
	JqSelfPop(Job);

	JqDecPrecondtion(Job, 1);

	// JqMutexLock lock(JqState.Mutex);
	// JqIncrementFinished(nJob);
}

uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos - 1].Job : 0;
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

void JqDumpState()
{
	printf("\n\nJqDumpState\n\n");

	printf("                                                                PreconditionCount\n");
	printf("                                          PendingFinish            Parent \n");
	printf("Reserved                                          PendingStart              Dependent             Queue     Next     \n");
	printf("  Base / Claimed  / Started  / Finished |               Queue|                     DepLink| Num        Waiters     Prev\n");
	printf("-------------------------------------------------------------------------------------------------------------------------\n");

	for(uint32_t i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		JqJob& Job = JqState.Jobs[i];
		if(Job.StartedHandle != Job.FinishedHandle || Job.StartedHandle != Job.ClaimedHandle)
		{
			uint8_t	 Queue;
			uint16_t PendingStart;
			JqUnpackStartAndQueue(Job.PendingStartAndQueue.load(), PendingStart, Queue);
			printf("%c %04x / %08x / %08x / %08x | %05d / %05d / %2x | %3d %08x %08x %04x | %3d / %02x / %02x / %04x / %04x\n", Job.Reserved ? 'R' : ' ', i,
				   Job.ClaimedHandle.load() / JQ_JOB_BUFFER_SIZE, Job.StartedHandle.load() / JQ_JOB_BUFFER_SIZE, Job.FinishedHandle.load() / JQ_JOB_BUFFER_SIZE, Job.PendingFinish.load(), PendingStart,
				   Queue, Job.PreconditionCount.load(), Job.Parent / JQ_JOB_BUFFER_SIZE, Job.DependentJob.Job / JQ_JOB_BUFFER_SIZE, Job.DependentJob.Next, Job.NumJobs, Job.Queue, Job.Waiters,
				   Job.Next, Job.Prev);
		}
	}
}
