#include "jqpipe.h"
#include <stdio.h>
int64_t JqTick();
int64_t JqTicksPerSecond();
// std::atomic<JqJobState> State;

bool JqPipeCancel(JqPipe* pPipe, JqPipeHandle PipeHandle)
{
	uint32_t Handle = PipeHandle.HandleInt;
	uint16_t nJobIndex = Handle % JQ_PIPE_BUFFER_SIZE;
	JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];
	JqJobState State;
	do
	{
		State = JqJobStateLoad(pJob);
		if(Handle == State.nStartedHandle && State.nNumStarted == 0)
		{
			JqJobState NewState = State;
			NewState.nNumStarted = NewState.nNumFinished = NewState.nNumJobs;
			NewState.nFinishedHandle = NewState.nStartedHandle;
			if(JqJobStateCompareAndSwap(pJob, NewState, State))
			{
				JqPipeHandle HandleX;
				HandleX.HandleInt = Handle;
				HandleX.Invalid = 0;
				HandleX.Pipe = pPipe->nPipeId;
				pPipe->FinishJobFunc(HandleX, State.nExternalId, State.nNumJobs, 1);
				return true;
			}
		}
		else
		{
			return false;
		}
	}while(1);
}

JqPipeHandle JqPipeAdd(JqPipe* pPipe, uint32_t nExternalId, int nNumJobs, int nRange)
{
	JQ_ASSERT(nNumJobs);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	JQ_ASSERT(nNumJobs > 0);
	uint64_t nNextHandle = 0;
	uint64_t nHandleRetries = 0;
	JqPipeHandle H;
	int Success = 0;
	do
	{
		
		nNextHandle = pPipe->nHandle.fetch_add(1);
		uint64_t HandleU = nNextHandle;
		nNextHandle &= 0xffffffff;
		if(nNextHandle)
		{
			nHandleRetries++;
			uint16_t nJobIndex = nNextHandle % JQ_PIPE_BUFFER_SIZE;
			JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];
			JqJobState State;
			{
				State = JqJobStateLoad(pJob);
				if(State.nStartedHandle == State.nFinishedHandle &&
				   State.nNumJobs == State.nNumStarted &&
				   State.nNumJobs == State.nNumFinished)
				{
					JqJobState New;
					New.nNumJobs = (uint32_t)nNumJobs;
					New.nNumStarted = 0;
					New.nExternalId = nExternalId;
					New.nStartedHandle = nNextHandle;
					New.nNumFinished = 0;
					New.nRange = (uint32_t)nRange;
					New.nFinishedHandle =  State.nFinishedHandle;
					if(JqJobStateCompareAndSwap(pJob, New, State))
					{
						Success = 1;
						H.Handle = nNextHandle;
						H.Invalid = 0;
						H.Pipe = pPipe->nPipeId;

					}
				}
			}
		}
		//unfortunately this is the best way i can come up with solving the reused handle problem.
		while(1)
		{
			uint64_t H = pPipe->nPut.load();
			if(H == HandleU)
			{
				uint64_t nNext = HandleU+1;
				if(pPipe->nPut.compare_exchange_strong(H, nNext))
				{
					break;
				}
			}

		}
		if(Success)
		{
			return H;
		}
	}while(1);
}


//internal
bool JqPipeStartInternal(JqPipe* pPipe, uint32_t nHandleInternal, uint16_t* pSubJob, bool* pLast)
{
	uint16_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
	JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];
	JqJobState State;
	do
	{
		State = JqJobStateLoad(pJob);
		if(State.nStartedHandle == nHandleInternal && State.nNumJobs > State.nNumStarted)
		{
			JqJobState NewState = State;
			NewState.nNumStarted = State.nNumStarted+1;
			bool bLast = State.nNumStarted + 1 == State.nNumJobs;
			if(JqJobStateCompareAndSwap(pJob, NewState, State))
			{
				*pSubJob = State.nNumStarted;
				*pLast = bLast;
				// pthread_threadid_np(NULL, &pJob->nThreadId);
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
	uint16_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
	JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];
	JqJobState State;
	do
	{
		State = JqJobStateLoad(pJob);
		JQ_ASSERT(State.nStartedHandle == nHandleInternal);
		JQ_ASSERT(State.nNumJobs > State.nNumFinished);
		JqJobState NewState = State;
		NewState.nNumFinished = State.nNumFinished+1;
		bool bLast = NewState.nNumFinished == State.nNumJobs;
		if(bLast)
		{
			//a note about race: since this is the last job, we can safely assume noone touches the internals.
			//and have plenty time to do this callback.
			JqPipeHandle Handle;
			Handle.HandleInt = nHandleInternal;
			Handle.Invalid = 0;
			Handle.Pipe = pPipe->nPipeId;
			pPipe->FinishJobFunc(Handle, State.nExternalId, State.nNumJobs, 0 );
			NewState.nFinishedHandle = State.nStartedHandle;
		}
		if(JqJobStateCompareAndSwap(pJob, NewState, State))
		{
			return bLast;
		}
	}while(1);
}


EJqPipeExecuteResult JqPipeExecuteInternal(JqPipe* pPipe, JqPipeHandle* pFinishedHandleOut, uint32_t nHandleInternal)
{
	uint16_t nSubJob;
	bool bIsLast = false;
	if(JqPipeStartInternal(pPipe, nHandleInternal, &nSubJob, &bIsLast))
	{
		{
			//JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
			uint16_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
			JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];

			JqJobState State = JqJobStateLoad(pJob);
			JqPipeHandle Handle;			
			JQ_ASSERT(State.nNumFinished < State.nNumJobs);
			Handle.HandleInt = nHandleInternal;
			Handle.Invalid = 0;
			Handle.Pipe = pPipe->nPipeId;
			pPipe->StartJobFunc(Handle, State.nExternalId, nSubJob, State.nNumJobs, State.nRange);
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

	return EJQ_EXECUTE_FAIL;

}


EJqPipeExecuteResult JqPipeExecute(JqPipe* pPipe, JqPipeHandle ExecuteHandle)
{
	JqPipeHandle foo;
	if(0 != ExecuteHandle.HandleInt)
	{
		return JqPipeExecuteInternal(pPipe, &foo, ExecuteHandle.HandleInt);
	}
	else
	{
		uint64_t nPut = pPipe->nPut.load();
		uint64_t nGet = pPipe->nGet.load();
		while(JQ_LT_WRAP(nGet, nPut))
		{
			JQ_ASSERT((nGet&0xffffffff) != 0);
			JqPipeHandle H;
			H.HandleInt = nGet & 0xffffffff;
			H.Invalid = 0;
			H.Pipe = pPipe->nPipeId;
			EJqPipeExecuteResult eRes = JqPipeExecuteInternal(pPipe, &foo, H.HandleInt);
			if(eRes != EJQ_EXECUTE_FAIL)
			{
				return eRes;
			}
			else
			{
				if(JqPipeIsAllStarted(pPipe, H))
				{
					uint64_t nGetNew = nGet+1;
					if(0 == (nGetNew&0xffffffff))
						nGetNew++;
					JQ_ASSERT(JQ_LE_WRAP(nGetNew,nPut));
					pPipe->nGet.compare_exchange_weak(nGet, nGetNew);
				}
			}
			nPut = pPipe->nPut.load();
			nGet = pPipe->nGet.load();
		}
	}
	return EJQ_EXECUTE_FAIL;
}
bool JqPipeIsDone(JqPipe* pPipe, JqPipeHandle nJob)
{
	JQ_ASSERT(nJob.Pipe == pPipe->nPipeId);

	uint64_t nHandleInternal = nJob.HandleInt;
	uint32_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
	
	uint64_t nFinished = JqJobStateLoad(&pPipe->Jobs[nJobIndex]).nFinishedHandle;
	return JQ_LE_WRAP_SHIFT(nHandleInternal, nFinished, 32);
}

bool JqPipeIsAllStarted(JqPipe* pPipe, JqPipeHandle nJob)
{
	JQ_ASSERT(nJob.Pipe == pPipe->nPipeId);

	uint64_t nHandleInternal = nJob.HandleInt;
	uint32_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
	JqJobState State = JqJobStateLoad(&pPipe->Jobs[nJobIndex]);
	uint64_t nFinished = State.nFinishedHandle;
	uint64_t nStarted = State.nStartedHandle;

	bool bFinished = JQ_LE_WRAP_SHIFT(nHandleInternal, nFinished, 32);
	bool bStarted = !JQ_LT_WRAP_SHIFT(nStarted, nHandleInternal, 32);
	bool bStartedAndFinished = bStarted && State.nNumStarted == State.nNumJobs ;

	return  bFinished || bStartedAndFinished; 

}

#include <stdio.h>

void JqPipeDump(FILE* F, JqPipe* pPipe)
{
	fprintf(F, "***** PIPE DUMP BEGIN %d\n", pPipe->nPipeId);
	fprintf(F, " Put %lld\n", pPipe->nPut.load());
	fprintf(F, " Get %lld\n", pPipe->nGet.load());
	for(uint32_t i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
	{
		JqPipeJob* pJob = &pPipe->Jobs[i];
		JqJobState State = JqJobStateLoad(pJob);

		if(State.nStartedHandle != State.nFinishedHandle ||
			State.nNumStarted != State.nNumJobs || 
			State.nNumFinished != State.nNumJobs)
		{
			fprintf(F, " JOB %04d :: ext %lld (%3lld/%3lld/%3lld) range %lld, (%lld/%lld)\n", 
				i, State.nExternalId,
				State.nNumStarted,
				State.nNumFinished,
				State.nNumJobs,
				State.nRange,
				State.nStartedHandle,
				State.nFinishedHandle);
		}
	}

	fprintf(F, "***** PIPE DUMP END   %d\n", pPipe->nPipeId);


}
