#pragma once

// - as soon as a job is added, it can be executed
// - if a job is added from inside a job it becomes a child job
// - When waiting for a job you also wait for all children
//
// -index vs handle
// -job vs work
// lock assertions
// microprofile thread shit
// test for non-function
// 

#ifndef JQ_WORK_BUFFER_SIZE
#define JQ_WORK_BUFFER_SIZE (1024)
#endif

#ifndef JQ_PRIORITY_MAX
#define JQ_PRIORITY_MAX 8
#endif

#ifndef JQ_MAX_JOB_STACK
#define JQ_MAX_JOB_STACK 8
#endif

#ifndef JQ_DEFAULT_WAIT_TIME_US
#define JQ_DEFAULT_WAIT_TIME_US 100
#endif

#ifndef JQ_BUFFER_FULL_USLEEP
#define JQ_BUFFER_FULL_WAIT_TIME_US 100
#endif

#ifdef JQ_NO_STD_FUNCTION
typedef void (*JqFunction)(void*, int, int);
#else
#include <functional>
typedef std::function<void (int,int) > JqFunction;
#endif


#include <stddef.h>
#include <stdint.h>


#define JQ_INVALID_PARENT 0 

//which jobs to execute when waiting
#define JQ_WAITFLAG_EXECUTE_SUCCESSORS 0x1
#define JQ_WAITFLAG_EXECUTE_ANY 0x2
#define JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS 0x3

//what to do when out of jobs
#define JQ_WAITFLAG_SPIN 0x4
#define JQ_WAITFLAG_SLEEP 0x8



#ifdef JQ_NO_STD_FUNCTION
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs = 1, int nRange = -1);
#else
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs = 1, int nRange = -1);
#endif

void JqWait(uint64_t nJob, uint32_t nWaitFlag = JQ_WAITFLAG_EXECUTE_SUCCESSORS | JQ_WAITFLAG_SLEEP, uint32_t usWaitTime = JQ_DEFAULT_WAIT_TIME_US);
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
#define JQ_USLEEP(us) usleep(us);
int64_t JqTicksPerSecond()
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
int64_t JqTick()
{
	return mach_absolute_time();
}
#elif defined(_WIN32)
#define JQ_BREAK() __debugbreak()
#define JQ_THREAD_LOCAL __declspec(thread)
#define JQ_STRCASECMP _stricmp
typedef uint32_t ThreadIdType;
#define JQ_USLEEP(us) JqUsleep(us);
int64_t JqTicksPerSecond()
{
	static int64_t nTicksPerSecond = 0;	
	if(nTicksPerSecond == 0) 
	{
		QueryPerformanceFrequency((LARGE_INTEGER*)&nTicksPerSecond);
	}
	return nTicksPerSecond;
}
int64_t JqTick()
{
	int64_t ticks;
	QueryPerformanceCounter((LARGE_INTEGER*)&ticks);
	return ticks;
}
void JqUsleep(__int64 usec) 
{ 
	if(usec > 20000)
	{
		Sleep((DWORD)(usec/1000));
	}
	else if(usec >= 1000)
	{
		timeBeginPeriod(1);
		Sleep((DWORD)(usec/1000));
		timeEndPeriod(1);
	}
	else
	{
		Sleep(0);
	}
}
#endif

#ifdef JQ_NO_ASSERT
#define JQ_ASSERT(a) do{}while(0)
#define JQ_ASSERT(a) do{}while(0)
#define JQ_ASSERT_NOT_LOCKED() do{}while(0)
#else
#define JQ_ASSERT(a) do{if(!(a)){JQ_BREAK();} }while(0)
#define JQ_ASSERT_NOT_LOCKED() do{}while(0)
#define JQ_ASSERT_LOCKED() do{}while(0)
#endif

#define JQ_NUM_JOBS (JQ_WORK_BUFFER_SIZE-1)

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


void JqStart(int nNumWorkers);
void JqStop();
void JqCheckFinished(uint64_t nWorkHandle);
uint32_t JqIncrementStarted(uint64_t nWorkHandle);
void JqIncrementFinished(uint64_t nWorkIndex);
void JqExecuteJob(uint64_t nWorkHandle, uint16_t nSubJob);
uint32_t JqTakeJob(uint16_t* pWorkIndexOut);
uint32_t JqTakeChildJob(uint64_t nWorkHandle, uint16_t* pWorkIndexOut);
void JqWorker(int nThreadId);
uint64_t JqNextHandle(uint64_t nHandle);
uint64_t JqDetachChild(uint64_t nChild);
void JqWaitAll();
void JqPriorityListAdd(uint32_t nIndex);
void JqPriorityListRemove(uint32_t nIndex);

JQ_THREAD_LOCAL uint64_t JqSelfStack[JQ_MAX_JOB_STACK] = {0};
JQ_THREAD_LOCAL uint32_t JqSelfPos = 0;

#if 1
#define uprintf(...) do{}while(0)
#else
#define uprintf printf
#endif


struct JqWorkEntry
{
	uint64_t nStartedHandle;
	uint64_t nFinishedHandle;

	uint16_t nNumJobs;
	uint16_t nNumStarted;
	uint16_t nNumFinished;
	
	JqFunction Function;
#ifdef JQ_NO_STD_FUNCTION
	void* pArg;
#endif

	uint8_t nPrio;

	int32_t nRange;

	uint16_t nLinkNext;
	uint16_t nLinkPrev;

	uint16_t nParent;
	uint16_t nFirstChild;
	uint16_t nSibling;

#ifdef JQ_ASSERT_SANITY
	int nTag;
#endif
};

struct JqState_t
{
	std::mutex Mutex;
	std::condition_variable Cond;
	int32_t nReleaseCount;
	std::thread* pWorkerThreads;

	
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


template<typename T>
struct JqMutexLock
{
	T& Mutex;
	JqMutexLock(T& Mutex)
	:Mutex(Mutex)
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexLock", 0x992233);
		Mutex.lock();
	}
	~JqMutexLock()
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("MutexUnlock", 0x992233);
		Mutex.unlock();
	}
};


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
		JqMutexLock<std::mutex> lock(JqState.Mutex);
		JqState.nReleaseCount += JqState.nNumWorkers;
	}
	JqState.Cond.notify_all();
	for(int i = 0; i < JqState.nNumWorkers; ++i)
	{
		JqState.pWorkerThreads[i].join();
	}
	delete[] JqState.pWorkerThreads;
	JqState.pWorkerThreads = 0;
	JqState.nNumWorkers = 0;

}

void JqCheckFinished(uint64_t nWorkHandle)
{
	JQ_ASSERT_LOCKED();
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JQ_ASSERT(nWorkHandle >= JqState.Work[nWork].nFinishedHandle);
	JQ_ASSERT(nWorkHandle == JqState.Work[nWork].nStartedHandle);
	if(0 == JqState.Work[nWork].nFirstChild && JqState.Work[nWork].nNumFinished == JqState.Work[nWork].nNumJobs)
	{
		uint16_t nParent = JqState.Work[nWork].nParent;
		if(nParent)
		{
			uint64_t nParentHandle = JqDetachChild(nWorkHandle);
			JqCheckFinished(nParentHandle);
		}
		JqState.Work[nWork].nFinishedHandle = JqState.Work[nWork].nStartedHandle;
		JqState.nFreeJobs++;
	}
}

uint32_t JqIncrementStarted(uint64_t nWorkHandle)
{
	JQ_ASSERT_LOCKED();
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
	JQ_MICROPROFILE_VERBOSE_SCOPE("Increment Finished", 0xffff);
	JqMutexLock<std::mutex> lock(JqState.Mutex);
	uint32_t nWork = nWorkHandle % JQ_WORK_BUFFER_SIZE; 
	JqState.Work[nWork].nNumFinished++;
	JqCheckFinished(nWorkHandle);
}


void JqAttachChild(uint64_t nParent, uint64_t nChild)
{
	uint16_t nParentIndex = nParent % JQ_WORK_BUFFER_SIZE;
	uint16_t nChildIndex = nChild % JQ_WORK_BUFFER_SIZE;
	uprintf("attach %d to %d\n", nChildIndex, nParentIndex);
	uint16_t nFirstChild = JqState.Work[nParentIndex].nFirstChild;
	JqState.Work[nChildIndex].nParent = nParentIndex;
	JqState.Work[nChildIndex].nSibling = nFirstChild;
	JqState.Work[nParentIndex].nFirstChild = nChildIndex;


	JQ_ASSERT(JqState.Work[nParentIndex].nFinishedHandle != nParent);
	JQ_ASSERT(JqState.Work[nParentIndex].nStartedHandle == nParent);


}

uint64_t JqDetachChild(uint64_t nChild)
{
	uint16_t nChildIndex = nChild % JQ_WORK_BUFFER_SIZE;
	uint16_t nParentIndex = JqState.Work[nChildIndex].nParent;
	JQ_ASSERT(JqState.Work[nChildIndex].nNumFinished == JqState.Work[nChildIndex].nNumJobs);
	JQ_ASSERT(JqState.Work[nChildIndex].nNumFinished == JqState.Work[nChildIndex].nNumStarted);
	if(0 == nParentIndex)
	{
		uprintf("not detaching 0 parent\n");
		return 0;
	}
	uprintf("detach %d from %d\n", nChildIndex, nParentIndex);

	JQ_ASSERT(nParentIndex != 0);
	JQ_ASSERT(JqState.Work[nChildIndex].nFirstChild == 0);
	uint16_t* pChildIndex = &JqState.Work[nParentIndex].nFirstChild;
	JQ_ASSERT(JqState.Work[nParentIndex].nFirstChild != 0); 
	while(*pChildIndex != nChildIndex)
	{
		JQ_ASSERT(JqState.Work[*pChildIndex].nSibling != 0);
		pChildIndex = &JqState.Work[*pChildIndex].nSibling;

	}
	JQ_ASSERT(pChildIndex);
	*pChildIndex = JqState.Work[nChildIndex].nSibling;
	return JqState.Work[nParentIndex].nStartedHandle;
}

//splits range evenly to jobs
int JqGetRangeStart(int nIndex, int nNumJobs, int nRange)
{
	int nFraction = nRange / nNumJobs;
	int nRemainder = nRange - nFraction * nNumJobs;
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


void JqExecuteJob(uint64_t nWorkHandle, uint16_t nSubJob)
{
	JQ_MICROPROFILE_SCOPE("ExecuteJob", 0xff);
	JQ_ASSERT_NOT_LOCKED();
	JQ_ASSERT(JqSelfPos < JQ_MAX_JOB_STACK);
	JqSelfStack[JqSelfPos++] = nWorkHandle;
	uint32_t nWorkIndex = nWorkHandle % JQ_WORK_BUFFER_SIZE;
	uint32_t nNumJobs = JqState.Work[nWorkIndex].nNumJobs;
	uint32_t nRange = JqState.Work[nWorkIndex].nRange;
	int nStart = JqGetRangeStart(nSubJob, nNumJobs, nRange);
	int nEnd = JqGetRangeStart(nSubJob+1, nNumJobs, nRange);
#ifdef JQ_NO_STD_FUNCTION
	JqState.Work[nWorkIndex].Function(JqState.Work[nWorkIndex].pArg, nSubJob);
#else
	JqState.Work[nWorkIndex].Function(nStart, nEnd);
#endif
	JqSelfPos--;
	JqIncrementFinished(nWorkHandle);
}


uint32_t JqTakeJob(uint16_t* pWorkIndexOut)
{
	JQ_ASSERT_LOCKED();
	for(int i = 0; i < JQ_PRIORITY_MAX; ++i)
	{
		uint32_t nWork = JqState.nPrioListHead[i];
		if(nWork)
		{
			*pWorkIndexOut = JqIncrementStarted(JqState.Work[nWork].nStartedHandle);
			return nWork;
		}
	}
	return 0;
}
#ifdef JQ_ASSERT_SANITY
void JqTagChildren(uint16_t nRoot)
{
	for(int i = 1; i < JQ_WORK_BUFFER_SIZE; ++i)
	{
		JQ_ASSERT(JqState.Work[i].nTag == 0);
		if(nRoot == i)
			JqState.Work[i].nTag = 1;
		else
		{
			int nParent = JqState.Work[i].nParent;
			while(nParent)
			{
				if(nParent == nRoot)
				{
					JqState.Work[i].nTag = 1;
					break;
				}
				nParent = JqState.Work[nParent].nParent;
			}
		}
	}
}
void JqLoopChildren(uint16_t nRoot)
{
	int nNext = JqState.Work[nRoot].nFirstChild;
	while(nNext != nRoot && nNext)
	{
		while(JqState.Work[nNext].nFirstChild)
			nNext = JqState.Work[nNext].nFirstChild;
		JQ_ASSERT(JqState.Work[nNext].nTag == 1);
		JqState.Work[nNext].nTag = 0;
		if(JqState.Work[nNext].nSibling)
			nNext = JqState.Work[nNext].nSibling;
		else
		{
			//search up
			nNext = JqState.Work[nNext].nParent;
			while(nNext != nRoot)
			{
				JQ_ASSERT(JqState.Work[nNext].nTag == 1);
				JqState.Work[nNext].nTag = 0;
				if(JqState.Work[nNext].nSibling)
				{
					nNext = JqState.Work[nNext].nSibling;
					break;
				}
				else
				{
					nNext = JqState.Work[nNext].nParent;
				}

			}
		}
	}

	JQ_ASSERT(JqState.Work[nRoot].nTag == 1);
	JqState.Work[nRoot].nTag = 0;
}
#endif
uint32_t JqTakeChildJob(uint64_t nJob, uint16_t* pIndexOut)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeChildJob", 0xff);
	JQ_ASSERT_LOCKED();
	#if JQ_ASSERT_SANITY
	{
		for(int i = 0; i < JQ_WORK_BUFFER_SIZE; ++i)
		{
			JqState.Work[i].nTag = 0;
		}
		JqTagChildren(nJob%JQ_WORK_BUFFER_SIZE);
		JqLoopChildren(nJob%JQ_WORK_BUFFER_SIZE);
	}
	#endif

	int16_t nRoot = nJob % JQ_WORK_BUFFER_SIZE;
	int16_t nWork = 0;


	int nNext = JqState.Work[nRoot].nFirstChild;
	while(nNext != nRoot && nNext && nWork == 0)
	{
		while(JqState.Work[nNext].nFirstChild)
			nNext = JqState.Work[nNext].nFirstChild;
		if(JqState.Work[nNext].nNumStarted < JqState.Work[nNext].nNumJobs)
		{
			nWork = nNext;
			break;
		}
		if(JqState.Work[nNext].nSibling)
			nNext = JqState.Work[nNext].nSibling;
		else
		{
			//search up
			nNext = JqState.Work[nNext].nParent;
			while(nNext != nRoot)
			{
				if(JqState.Work[nNext].nNumStarted < JqState.Work[nNext].nNumJobs)
				{
					nWork = nNext;
					break;
				}

				if(JqState.Work[nNext].nSibling)
				{
					nNext = JqState.Work[nNext].nSibling;
					break;
				}
				else
				{
					nNext = JqState.Work[nNext].nParent;
				}

			}
		}
	}

	if(0 == nWork && JqState.Work[nRoot].nNumStarted < JqState.Work[nRoot].nNumJobs)
	{
		nWork = nRoot;
	}
	JQ_ASSERT(0 == nWork || JqState.Work[nWork].nNumStarted < JqState.Work[nWork].nNumJobs);
	if(nWork)
	{
		*pIndexOut = JqIncrementStarted(JqState.Work[nWork].nStartedHandle);
		return nWork;
	}
	else
	{
		return 0;
	}
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
			while(!JqState.nReleaseCount)
			{
				JqState.Cond.wait(lock);
			}
			JqState.nReleaseCount--;
			uprintf("worker woke up %d\n", nThreadId);
			nSubIndex = 0;
			nWork = JqTakeJob(&nSubIndex);

		}
		while(nWork)
		{
			uprintf("worker %d executing %d,%d\n", nThreadId, nWork, nSubIndex);
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubIndex);
			
			JqMutexLock<std::mutex> lock(JqState.Mutex);
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


#ifdef JQ_NO_STD_FUNCTION
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, void* pArg, int nNumJobs, int nRange)
#else
uint64_t JqAdd(JqFunction JobFunc, uint8_t nPrio, int nNumJobs, int nRange)
#endif
{
	JQ_ASSERT(nPrio < JQ_PRIORITY_MAX);
	JQ_ASSERT(nNumJobs);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	uint64_t nNextHandle = 0;
	{
		JqMutexLock<std::mutex> lock(JqState.Mutex);
		while(!JqState.nFreeJobs)
		{
			if(JqSelfPos < JQ_MAX_JOB_STACK)
			{
				JQ_MICROPROFILE_SCOPE("JqAdd Executing Job", 0xff0000); // if this starts happening the job queue size should be increased..
				uint16_t nSubIndex = 0;
				uint32_t nWork = JqTakeJob(&nSubIndex);
				if(nWork)
				{
					JqState.Mutex.unlock();
					uprintf("JqAdd: job queue full. executing %d,%d\n", nWork, nSubIndex);
					JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubIndex);
					JqState.Mutex.lock();
				}
			}
			else
			{
				JQ_BREAK(); //out of job queue space. increase JQ_WORK_BUFFER_SIZE or create fewer jobs
			}
		}
		JQ_ASSERT(JqState.nFreeJobs != 0);
		nNextHandle = JqState.nNextHandle;
		uint64_t nHandleIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
		uint32_t nCount = 0;
		while(!JqIsDone(nNextHandle))
		{
			nNextHandle = JqNextHandle(nNextHandle);
			nHandleIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
			JQ_ASSERT(nCount++ < JQ_WORK_BUFFER_SIZE);
		}
		JqState.nNextHandle = JqNextHandle(nNextHandle);
		JqState.nFreeJobs--;

		JqWorkEntry* pEntry = &JqState.Work[nHandleIndex];
		JQ_ASSERT(pEntry->nFinishedHandle < nNextHandle);
		pEntry->nStartedHandle = nNextHandle;
		pEntry->nNumJobs = nNumJobs;
		pEntry->nNumStarted = 0;
		pEntry->nNumFinished = 0;
		pEntry->nRange = nRange;
		uint64_t nParentHandle = JqSelf();
		pEntry->nParent = nParentHandle % JQ_WORK_BUFFER_SIZE;
		if(pEntry->nParent)
		{
			JqAttachChild(nParentHandle, nNextHandle);
		}
		else
		{

			uprintf("not attaching %llu to %d\n", nNextHandle, pEntry->nParent);

		}

		pEntry->Function = JobFunc;
		#ifdef JQ_NO_STD_FUNCTION
		pEntry->pArg = pArg;
		#endif
		pEntry->nPrio = nPrio;

		JqPriorityListAdd(nHandleIndex);
	}

	if(nNumJobs < JqState.nNumWorkers)
	{
		uprintf("notifying %d\n", nNumJobs);
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			JqState.nReleaseCount += nNumJobs;
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
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			JqState.nReleaseCount += JqState.nNumWorkers;
		}
		JqState.Cond.notify_all();
	}
	return nNextHandle;
}

bool JqIsDone(uint64_t nJob)
{
	uint64_t nHandleIndex = nJob % JQ_WORK_BUFFER_SIZE;
	JQ_ASSERT(JqState.Work[nHandleIndex].nFinishedHandle <= JqState.Work[nHandleIndex].nStartedHandle);
	return JqState.Work[nHandleIndex].nFinishedHandle == JqState.Work[nHandleIndex].nStartedHandle;
}


void JqWaitAll()
{
	while(JqState.nFreeJobs != JQ_NUM_JOBS)
	{
		uint32_t nWork = 0;
		uint16_t nSubWork = 0;
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubWork);
		}
		if(nWork)
		{
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubWork);
			uprintf("waitall executed job\n");
		}
		else
		{
			JQ_USLEEP(1000);
		}	

	}
}
JQ_THREAD_LOCAL int JqSpinloop = 0; //prevent optimizer from removing spin loop
void JqWait(uint64_t nJob, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	while(!JqIsDone(nJob))
	{
		uint32_t nWork = 0;
		uint16_t nSubWork = 0;
		if(nWaitFlag & JQ_WAITFLAG_EXECUTE_SUCCESSORS)
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeChildJob(nJob, &nSubWork);
		}
		
		if(0 == nWork && 0 != (nWaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			JqMutexLock<std::mutex> lock(JqState.Mutex);
			nWork = JqTakeJob(&nSubWork);
		}
		if(!nWork)
		{
			JQ_ASSERT(0 != (nWaitFlag& (JQ_WAITFLAG_SLEEP|JQ_WAITFLAG_SPIN)));
			if(nWaitFlag & JQ_WAITFLAG_SPIN)
			{
				uint64_t nTick = JqTick();
				uint64_t nTickEnd = nTick;
				uint64_t nTicksPerSecond = JqTicksPerSecond();
				do
				{
					int result = 0;
					for(int i = 0; i < 1000; ++i)
					{
						result |= i << (i^7); //do something.. whatever
					}
					JqSpinloop |= result; //write it somewhere so the optimizer can't remote it
				}while( (1000000ull*(JqTick()-nTick)) / nTicksPerSecond < nUsWaitTime);

			}
			else
			{
				JQ_USLEEP(nUsWaitTime);
			}
		}
		else
		{
			JqExecuteJob(JqState.Work[nWork].nStartedHandle, nSubWork);
		}

	}
}

uint64_t JqSelf()
{
	return JqSelfPos ? JqSelfStack[JqSelfPos-1] : 0;
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