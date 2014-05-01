#pragma once

//
// - jobs are added, and executed
// - as soon as a job is added, it can be executed
// - When waiting for a job you also wait for all children
// - child jobs should only be added from the running job
//
//
// -child find algorithm
// -yield yada yada
//
#ifndef JQ_WORK_BUFFER_SIZE
#define JQ_WORK_BUFFER_SIZE 1024
#endif

#ifndef JQ_PRIORITY_MAX
#define JQ_PRIORITY_MAX 8
#endif

#ifndef JQ_MAX_JOB_STACK
#define JQ_MAX_JOB_STACK 8
#endif



#include <functional>
#include <stddef.h>
#include <stdint.h>

typedef std::function<void (void*,int) > JqFunction;

#define JQ_INVALID_PARENT 0 
//which jobs to execute when waiting
#define JQ_WAITFLAG_EXECUTE_SUCCESSORS 0x1
#define JQ_WAITFLAG_EXECUTE_ANY 0x2
#define JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS 0x3
//what to do when out of jobs
#define JQ_WAITFLAG_SPIN 0x4
#define JQ_WAITFLAG_YIELD 0x8




uint64_t JqAdd(JqFunction JobFunc, void* pArg, uint8_t nPrio, int nNumJobs, uint64_t nParent = JQ_INVALID_PARENT);
void JqWait(uint64_t nJob, int nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_YIELD);
bool JqIsDone(uint64_t nJob);
uint64_t JqSelf();



void JqStart(int nNumWorkers);
void JqStop();





#ifdef JQ_IMPL
#if defined(__APPLE__)
#define JQ_BREAK() __builtin_trap()
#define JQ_THREAD_LOCAL __thread
#define JQ_STRCASECMP strcasecmp
typedef uint64_t ThreadIdType;
#define JQ_SLEEP(us) usleep(us);
#elif defined(_WIN32)
#define JQ_BREAK() __debugbreak()
#define JQ_THREAD_LOCAL __declspec(thread)
#define JQ_STRCASECMP _stricmp
typedef uint32_t ThreadIdType;
#endif

#define JQ_ASSERT(a) do{if(!(a)){JQ_BREAK();} }while(0)
#define JQ_ASSERT_NOT_LOCKED() do{}while(0)
#define JQ_ASSERT_LOCKED() do{}while(0)
#define JQ_NUM_JOBS (JQ_WORK_BUFFER_SIZE-1)


void JqStart(int nNumWorkers);
void JqStop();
void JqCheckFinished(uint64_t nWorkHandle);
uint32_t JqIncrementStarted(uint64_t nWorkHandle);
void JqIncrementFinished(uint64_t nWorkIndex);
void JqIncrementRefCount(uint64_t nWorkHandle);
void JqDecrementRefCount(uint64_t nWorkHandle);
void JqExecuteJob(uint64_t nWorkHandle, uint16_t nSubJob);
uint32_t JqTakeJob(uint16_t* pWorkIndexOut);
void JqWorker(int nThreadId);
uint64_t JqNextHandle(uint64_t nHandle);
bool JqCanReuse(uint64_t nJob);
void JqWaitAll();
void JqPriorityListAdd(uint32_t nIndex);
void JqPriorityListRemove(uint32_t nIndex);

JQ_THREAD_LOCAL uint64_t JqSelfStack[JQ_MAX_JOB_STACK] = {0};
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;
#define uprintf(...) do{}while(0)

enum JqJobState
{
	JqIdle = 0,
	JqRunning,
	JqFinished,
};

struct JqWorkEntry
{
	uint64_t nStartedHandle;
	uint64_t nFinishedHandle;

	uint16_t nNumJobs;
	uint16_t nNumStarted;
	uint16_t nNumFinished;

	uint32_t nRefCount;
	uint64_t nParentJob;
	
	JqFunction Function;
	void* pArg;

	uint8_t nPrio;


	uint16_t nLinkNext;
	uint16_t nLinkPrev;
};

struct JqState_t
{
	std::mutex Mutex;
	std::condition_variable Cond;
	int32_t CondReleaseCount;
	std::thread* pWorkerThreads;
	std::mutex* WorkerMutex; //for waiting from jobs
	std::condition_variable* WorkerCondition;
	int nNumWorkers;
	int nStop;

	uint64_t nNextHandle;
	uint32_t nFreeJobs;

	JqWorkEntry Work[JQ_WORK_BUFFER_SIZE];
	uint16_t nPrioListHead[JQ_PRIORITY_MAX];
	uint16_t nPrioListTail[JQ_PRIORITY_MAX];

	JqState_t()
	:nNumWorkers(0)
	{
	}
} JqState;




void JqStart(int nNumWorkers)
{
	JQ_ASSERT(JqState.nNumWorkers == 0);
	JqState.pWorkerThreads = new std::thread[nNumWorkers];
	JqState.nNumWorkers = nNumWorkers;
	JqState.nStop = 0;
	for(int i = 0; i < nNumWorkers; ++i)
	{
		JqState.pWorkerThreads[i] = std::thread(JqWorker, i);
	}
	memset(&JqState.Work, 0, sizeof(JqState.Work));
	memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.nFreeJobs = JQ_NUM_JOBS;
	JqState.nNextHandle = 1;
}

void JqStop()
{
	JqWaitAll();
	JqState.nStop = 1;
	uprintf("notify exit\n");
	{
		std::lock_guard<std::mutex> lock(JqState.Mutex);
		JqState.CondReleaseCount += JqState.nNumWorkers;
	}
	JqState.Cond.notify_all();
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JqState.pWorkerThreads[i].join();
	}
	delete[] JqState.pWorkerThreads;
	JqState.nNumWorkers = 0;

}

void JqCheckFinished(uint64_t nWorkHandle)
{
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(nWorkHandle >= JqState.Work[nWork].nFinishedHandle);
	JQ_ASSERT(nWorkHandle == JqState.Work[nWork].nStartedHandle);
	if(0 == JqState.Work[nWork].nRefCount && JqState.Work[nWork].nNumFinished == JqState.Work[nWork].nNumJobs)
	{
		JqState.Work[nWork].nFinishedHandle = JqState.Work[nWork].nStartedHandle;
		JqState.nFreeJobs++;
	}
}

uint32_t JqIncrementStarted(uint64_t nWorkHandle)
{
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(JqState.Work[nWork].nNumJobs > JqState.Work[nWork].nNumStarted);
	uint32_t nSubIndex = JqState.Work[nWork].nNumStarted++;
	if(JqState.Work[nWork].nNumStarted == JqState.Work[nWork].nNumJobs)
	{
		JqPriorityListRemove(nWork);
	}
	return nSubIndex;
}
void JqIncrementFinished(uint64_t nWorkHandle)
{
	std::lock_guard<std::mutex> lock(JqState.Mutex);

	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JqState.Work[nWork].nNumFinished++;
	JqCheckFinished(nWorkHandle);
}

void JqIncrementRefCount(uint64_t nWorkHandle)
{
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(JqState.Work[nWork].nFinishedHandle != nWorkHandle);
	JQ_ASSERT(JqState.Work[nWork].nStartedHandle == nWorkHandle);
	JqState.Work[nWork].nRefCount++;
}

void JqDecrementRefCount(uint64_t nWorkHandle)
{
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(JqState.Work[nWork].nFinishedHandle != nWorkHandle);
	JQ_ASSERT(JqState.Work[nWork].nStartedHandle == nWorkHandle);
	JqState.Work[nWork].nRefCount--;
	JqCheckFinished(nWorkHandle);
}


void JqExecuteJob(uint64_t nWorkHandle, uint16_t nSubJob)
{
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JqSelfStack[JqSelfPos++] = nWorkHandle;
	uint32_t nWorkIndex = nWorkHandle % JQ_WORK_BUFFER_SIZE;
	JqState.Work[nWorkIndex].Function(JqState.Work[nWorkIndex].pArg, nSubJob);
	JqSelfPos--;
	JqIncrementFinished(nWorkHandle);
}


uint32_t JqTakeJob(uint16_t* pWorkIndexOut)
{
	for(int i = 0; i < JQ_PRIORITY_MAX; ++i)
	{
		uint32_t nWork = JqState.nPrioListHead[i];
		if(nWork)
		{
			*pWorkIndexOut = JqIncrementStarted(nWork);
			return nWork;
		}
	}
	return 0;
}

void JqWorker(int nThreadId)
{
	uprintf("start JQ worker %d\n", nThreadId);
	while(0 == JqState.nStop)
	{
		uint32_t nWork = 0;
		uint16_t nSubIndex = 0;
		{
			uprintf("worker waiting for signal %d\n", nThreadId);
			std::unique_lock<std::mutex> lock(JqState.Mutex);
			while(!JqState.CondReleaseCount)
			{
				JqState.Cond.wait(lock);
			}
			JqState.CondReleaseCount--;
			uprintf("worker woke up %d\n", nThreadId);
			nSubIndex = 0;
			nWork = JqTakeJob(&nSubIndex);

		}
		while(nWork)
		{
			uprintf("worker %d executing %d,%d\n", nThreadId, nWork, nSubIndex);
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubIndex);
			
			std::unique_lock<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubIndex);
		}
	}
	uprintf("stop JQ worker %d\n", nThreadId);
}
uint64_t JqNextHandle(uint64_t nHandle)
{
	nHandle++;
	if(0 == (nHandle%JQ_WORK_BUFFER_SIZE))
	{
		nHandle++;
	}
	return nHandle;
}

uint64_t JqAdd(JqFunction JobFunc, void* pArg, uint8_t nPrio, int nNumJobs, uint64_t nParent)
{
	JQ_ASSERT(nPrio < JQ_PRIORITY_MAX);
	JQ_ASSERT(!JqState.nStop);
	uint64_t nNextHandle = 0;
	{
		std::lock_guard<std::mutex> lock(JqState.Mutex);
		JQ_ASSERT(JqState.nFreeJobs != 0);
		nNextHandle = JqState.nNextHandle;
		uint64_t nHandleIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
		while(!JqCanReuse(nNextHandle))
		{
			nNextHandle = JqNextHandle(nNextHandle);
			nHandleIndex = nNextHandle%JQ_WORK_BUFFER_SIZE;
		}
		JqState.nNextHandle = JqNextHandle(nNextHandle);
		JqState.nFreeJobs--;

		JqWorkEntry* pEntry = &JqState.Work[nHandleIndex];
		JQ_ASSERT(pEntry->nFinishedHandle < nNextHandle);
		pEntry->nStartedHandle = nNextHandle;
		pEntry->nNumJobs = nNumJobs;
		pEntry->nNumStarted = 0;
		pEntry->nNumFinished = 0;
		pEntry->nParentJob = nParent;
		if(nParent)
		{
			JqIncrementRefCount(nParent);
		}

		pEntry->Function = JobFunc;
		pEntry->pArg = pArg;
		pEntry->nPrio = nPrio;

		JqPriorityListAdd(nHandleIndex);
	}
			JqState.Cond.notify_all();

	if(nNumJobs < JqState.nNumWorkers)
	{
		uprintf("notifying %d\n", nNumJobs);
		{
			std::lock_guard<std::mutex> lock(JqState.Mutex);
			JqState.CondReleaseCount += nNumJobs;
		}
		for(int i = 0; i < nNumJobs; ++i)
		{
			JqState.Cond.notify_one();
		}
	}
	else
	{
		uprintf("notifying all\n");
		{
			std::lock_guard<std::mutex> lock(JqState.Mutex);
			JqState.CondReleaseCount += JqState.nNumWorkers;
		}
		JqState.Cond.notify_all();
	}
	return nNextHandle;
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nHandleIndex = nJob % JQ_WORK_BUFFER_SIZE;
	return JqState.Work[nHandleIndex].nFinishedHandle >= nJob;
}

bool JqCanReuse(uint64_t nJob)
{
	uint64_t nJobIndex = nJob % JQ_WORK_BUFFER_SIZE;
	return JqState.Work[nJobIndex].nNumFinished == JqState.Work[nJobIndex].nNumJobs && JqState.Work[nJobIndex].nRefCount == 0;
}

void JqWaitAll()
{
	while(JqState.nFreeJobs != JQ_NUM_JOBS)
	{
		uint32_t nWork = 0;
		uint16_t nSubWork = 0;
		{
			std::lock_guard<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubWork);
		}
		if(nWork)
		{
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubWork);
			uprintf("waitall executed job\n");
		}
		else
		{
			JQ_SLEEP(1000);
		}	

	}
}
void JqWait(uint64_t nJob, int nWaitFlag)
{
	while(!JqIsDone(nJob))
	{
		uint32_t nWork = 0;
		uint16_t nSubWork = 0;
		if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			//todo
			std::lock_guard<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubWork);
		}
		else if(nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY)
		{
			std::lock_guard<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubWork);
		}
		if(!nWork)
		{
			JQ_SLEEP(50);
			#if 0
			JQ_ASSERT(0 != (nWaitFlag& (JQ_WAITFLAG_YIELD|JQ_WAITFLAG_SPIN)));
			if(nWaitFlag & JQ_WAITFLAG_SPIN)
			{
				///spin
				for(int i = 0; i < 1000; ++i);
			}
			else
			{
				//yield or something
				JQ_BREAK();
			}
			#endif
		}
		else
		{
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubWork);
		}

	}
}

uint64_t JqSelf()
{
	JQ_ASSERT(JqSelfPos);
	return JqSelfStack[JqSelfPos-1];
}



void JqPriorityListAdd(uint32_t nIndex)
{
	uint8_t nPrio = JqState.Work[nIndex].nPrio;
	JQ_ASSERT(JqState.Work[nIndex].nLinkNext == 0);
	JQ_ASSERT(JqState.Work[nIndex].nLinkPrev == 0);
	uint16_t nTail = JqState.nPrioListTail[nPrio];
	if(nTail != 0)
	{
		JQ_ASSERT(JqState.Work[nTail].nLinkNext == 0);
		JqState.Work[nTail].nLinkNext = nIndex;
		JqState.Work[nIndex].nLinkPrev = nTail;
		JqState.Work[nIndex].nLinkNext = 0;
		JqState.nPrioListTail[nPrio] = nIndex;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListHead[nPrio] == 0);
		JqState.nPrioListHead[nPrio] = nIndex;
		JqState.nPrioListTail[nPrio] = nIndex;
		JqState.Work[nIndex].nLinkNext = 0;
		JqState.Work[nIndex].nLinkPrev = 0;
	}
}
void JqPriorityListRemove(uint32_t nIndex)
{
	uint8_t nPrio = JqState.Work[nIndex].nPrio;
	uint16_t nNext = JqState.Work[nIndex].nLinkNext;
	uint16_t nPrev = JqState.Work[nIndex].nLinkPrev;
	JqState.Work[nIndex].nLinkNext = 0;
	JqState.Work[nIndex].nLinkPrev = 0;

	if(nNext != 0)
	{
		JqState.Work[nNext].nLinkPrev = nPrev;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListTail[nPrio] == nIndex);
		JqState.nPrioListTail[nPrio] = nPrev;
	}
	if(nPrev != 0)
	{
		JqState.Work[nPrev].nLinkNext = nNext;
	}
	else
	{
		JQ_ASSERT(JqState.nPrioListHead[nPrio] == nIndex);
		JqState.nPrioListHead[nPrio] = nNext;
	}

}



#endif