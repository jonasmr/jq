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
// Lockless Job pipe

//only config option
#ifndef JQ_WORK_BUFFER_SIZE
#define JQ_WORK_BUFFER_SIZE (8192)
#endif

#ifndef JQ_CACHE_LINE_SIZE
#define JQ_CACHE_LINE_SIZE 64
#endif

#ifndef JQ_API
#define JQ_API
#endif

#ifndef JQ_LT_WRAP
#define JQ_LT_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<0)
#define JQ_LT_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<0)
#define JQ_LE_WRAP(a, b) (((int64_t)((uint64_t)a - (uint64_t)b))<=0)
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits)))<=0)
#endif


struct JQ_ALIGN_16 JqJobState
{
	uinion
	{
		struct
		{
			uint64_t Atomic[2];
		};
		struct
		{
			uint64_t nNumJobs 			: 10;
			uint64_t nNumStarted 		: 10;
			uint64_t nUnused 			: 12;
			uint64_t nStartedHandle 	: 32;
			
			uint64_t nNumFinished		: 10;
			uint64_t nRange		 		: 22;
			uint64_t nFinishedHandle 	: 32;

		};
	};
};

struct JqPipeJob
{
#ifdef __cplusplus
private:
	friend JqJobState JqJobStateLoad(JqPipeJob* pJob);
	friend bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState New, JqJobStateOld);
	JqJobState State;
public:
#else
	JqJobState State;
#endif
};


inline JqJobState JqJobStateLoad(JqPipeJob* pJob)
{
	return pJob->State;
}
inline bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old)
{
	JQ_ASSERT(0 == (0xf & (intptr)&New));
	JQ_ASSERT(0 == (0xf & (intptr)&Old));
	return 1 == _InterlockedCompareExchange128(pJob->State.Atomic, New.Atomic[0], New.Atomic[1], Old.Atomic);
}


struct JqPipeStats
{
	std::atomic<uint64_t> nJobsAdded;
	std::atomic<uint64_t> nJobsFinished;
	std::atomic<uint64_t> nSubJobsAdded;
	std::atomic<uint64_t> nSubJobsFinished;

};

void (JqPipeRunJob*)(JqPipeJob* pJob, JqPipeHandle Handle, uint16_t nSubJob, int nNumJobs, int nRange);

struct JQ_ALIGN_CACHELINE JqPipe
{
	std::atomic<uint64_t>	nPut;
	std::atomic<uint64_t>	nGet;
	uint8_t					nPipeId;
	JqJob					Jobs[JQ_WORK_BUFFER_SIZE];

	JqPipeRunJob			StartJobFunc;
	JqPipeStats				Stats;
};

struct JqPipeHandle
{
	union
	{
		uint64_t Handle;
		struct
		{
			uint64_t HandleInt 	: 32;
			uint64_t Pad0	 	: 24;
			uint64_t Pipe 		: 8;
		};
	};
};
enum EJqPipeExecuteResult
{
	EJQ_EXCUTE_FAIL,
	EJQ_EXECUTE_SUCCES,
	EJQ_EXECUTE_FINISHED,
};

JQ_API JqPipeHandle 		JqPipeAdd(JqPipe* pPipe, int nNumJobs, int nRange);
JQ_API JqPipeExecuteResult 	JqPipeExecute(JqPipe* pPipe, JqPipeHandle* pFinishedHandleOut, JqPipeHandle Job);
JQ_API bool 				JqPipeIsDone(JqPipe* pPipe, JqPipeHandle Job)

#ifdef JQ_PIPE_IMPL
JqPipeHandle JqPipeAdd(JqPipe* pPipe, int nNumJobs, int nRange)
{
	JQ_ASSERT(nNumJobs);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	if(nNumJobs < 0)
	{
		nNumJobs = JqState.nNumWorkers;
	}
	uint64_t nNextHandle = 0;
	do
	{
		nNextHandle = pPipe->nPut.fetch_add(1);
		nNextHandle = & 0xffffffff;
		if(nNextHandle)
		{
			uint16_t nJobIndex = nNextHandle % JQ_WORK_BUFFER_SIZE;
			JqJob* pJob = pPipe->Jobs[nJobIndex];
			JqJobState State;
			do
			{
				State = JqJobStateLoad(pJob);
				if(State.nStartedHandled == State.nFinishedHandle &&
				   State.nNumJobs == State.nNumStarted &&
				   State.nNumJobs == State.nNumFinished)
				{
					JqState New;
					New.nNumJobs = (uint32_t)nNumJobs;
					New.nNumStarted = 0;
					New.nUnused = 0;
					New.nStartedHandle = nNextHandle;
					New.nNumFinished = 0;
					New.nRange = (uint32_t)nRange;
					New.nFinishedHandle =  State.nFinishedHandle;
					if(JqJobStateCompareAndSwap(pJob, New, State))
					{
						return (nNextHandle<<8) | pPipe->nPipeId;
					}
				}
				else
				{
					break;//still running, bail out and take next
				}
			}while(1);
		}
	}while(1);
}


//internal
bool JqPipeStartInternal(JqPipe* pPipe, uint32_t nHandleInternal, uint16_t* pSubJob, bool* pLast)
{
	uint16_t nJobIndex = nGet % JQ_WORK_BUFFER_SIZE;
	JqJob* pJob = pPipe->Jobs[nJobIndex];
	JqJobState State;
	do
	{
		State = JqJobStateLoad(pJob);
		if(State.nStartedHandle == nHandleInternal && State.nNumJobs > State.nNumStarted)
		{
			JqJobState NewState = State;
			NewState.nNumStarted = State.nNumstarted+1;
			bool bLast = State.nNumstarted + 1 == State.nNumJobs;
			if(JqJobStateCompareAndSwap(pJob, NewState, State))
			{
				*pSubJob = NewState.nNumStarted;
				*pLast = bLast;
				return true;
			}
			else
			{
				continue;
			}

		}
		else
		{
			return false;
		}
	}while(1);
}

bool JqPipeFinishInternal(JqPipe* pPipe, uint32_t nHandleInternal)
{
	uint16_t nJobIndex = nGet % JQ_WORK_BUFFER_SIZE;
	JqJob* pJob = pPipe->Jobs[nJobIndex];
	JqJobState State;
	do
	{
		State = JqJobStateLoad(pJob);
		JQ_ASSERT(State.nStartedHandle == nHandleInternal)
		JQ_ASSERT(State.nNumJobs < State.nNumFinished)
		JqState NewState = State;
		NewState.nNumFinished = State.nNumFinished+1;
		bool bLast = NewState.nNumFinished == State.nNumJobs;
		if(JqJobStateCompareAndSwap(pJob, NewState, State))
		{
			return bLast;
		}
	}while(1);
}


JqPipeExecuteResult JqPipeExecuteInternal(JqPipe* pPipe, JqPipeHandle* pFinishedHandleOut, uint32_t nHandleInternal)
{
	uint16_t nSubJob;
	bool bIsLast = false;
	if(JqPipeStartInternal(pPipe, nHandleInternal, &nSubJob, &bLast))
	{
		if(bLast)
		{
			pPipe->nGet.fetch_add(1);
		}
		//execute.

		{
			JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
			JqState State = JqJobStateLoad(pJob);
			JqPipeHandle Handle;			
			Handle.HandleInt = nHandleInternal;
			Handle.Pad0 = 0;
			Handle.nPipeId = pPipe->nPipeId;
			pPipe->StartJobFunc(pJob, HandleInternal, nPipeId, nSubJob, State.nNumJobs, State.nRange);
		}
		//finish

		if(JqPipeFinishInternal(pPipe, nHandleInternal))
		{
			return EJQ_EXECUTE_FINISHED;
		}
		else
		{
			return EJQ_EXECUTE_SUCCES;
		}
	}

	return EJQ_EXCUTE_FAIL;

}


JqPipeExecuteResult JqPipeExecute(JqPipe* pPipe, JqPipeHandle* pFinishedHandleOut, JqPipeHandle ExecuteHandle);
{
	if(0 != ExecuteHandle.HandleInt)
	{
		return JqPipeExecuteInternal(pPipe, pFinishedHandleOut, Job.HandleInt);
	}
	else
	{
		uint64_t nPut = pPipe->nPut.load();
		uint64_t nGet = pPipe->nGet.load();
		while(nPut != nGet)
		{
			uint32_t nHandle = (uint32_t)nGet;
			JqPipeExecuteResult eRes = JqPipeExecuteInternal(pPipe, pFinishedHandleOut, ExecuteHandle.HandleInt);
			if(eRes != EJQ_EXCUTE_FAIL)
			{
				return eRes;
			}
			nPut = pPipe->nPut.load();
			nGet = pPipe->nGet.load();
		}
	}
	return EJQ_EXCUTE_FAIL;
}
bool JqPipeIsDone(JqPipe* pPipe, JqPipeHandle nJob)
{
	JQ_ASSERT(nJob.PipeId == pPipe->nId);

	uint64_t nHandleInternal = nJob.nHandleInt;
	uint32_t nJobIndex = nHandleInternal % JQ_WORK_BUFFER_SIZE;
	
	uint64_t nFinished = JqJobStateLoad(&pPipe->Jobs[nJobIndex]).nFinishedHandle;
	return JQ_LE_WRAP_SHIFT(nHandleInternal, nFinished, 32);
}

#endif
