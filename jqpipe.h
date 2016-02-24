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
#ifndef JQ_PIPE_BUFFER_SIZE
#define JQ_PIPE_BUFFER_SIZE (2048)
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
#define JQ_LE_WRAP_SHIFT(a, b, bits) (((int64_t)((uint64_t)(a<<(bits)) - (uint64_t)(b<<(bits))))<=0)
#endif

//hack
#define JQ_BREAK() __builtin_trap()

#ifdef JQ_NO_ASSERT
#define JQ_ASSERT(a) do{}while(0)
#else
#define JQ_ASSERT(a) do{if(!(a)){JQ_BREAK();} }while(0)
#endif

#include <stddef.h>
#include <stdint.h>
#include <atomic>


#define JQ_PIPE_EXTERNAL_ID_BITS 12
struct JqJobState
{
	union
	{
		struct
		{
			uint64_t Atomic[2];
		};
		struct
		{
			uint64_t nNumJobs 			: 10;
			uint64_t nNumStarted 		: 10;
			uint64_t nExternalId 		: JQ_PIPE_EXTERNAL_ID_BITS; 
			uint64_t nStartedHandle 	: 32;
			
			uint64_t nNumFinished		: 10;
			uint64_t nRange		 		: 22;
			uint64_t nFinishedHandle 	: 32;

		};
	};
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

#include <atomic>

struct JqPipeJob
{
#ifdef __cplusplus
private:
	friend JqJobState JqJobStateLoad(JqPipeJob* pJob);
	friend bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old);
	#ifdef _WIN32
	JqJobState State;
	#else
	std::atomic<JqJobState> State;
	#endif
public:
#else
	JqJobState State;
#endif
};
#include <stdio.h>

#ifdef _WIN32
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
#else
inline JqJobState JqJobStateLoad(JqPipeJob* pJob)
{
	return pJob->State.load();
}
inline bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old)
{
	// JQ_ASSERT(0 == (0xf & (intptr_t)&New));
	// JQ_ASSERT(0 == (0xf & (intptr_t)&Old));
	//return 1 == _InterlockedCompareExchange128(pJob->State.Atomic, New.Atomic[0], New.Atomic[1], Old.Atomic);
	//JQ_ASSERT(New.nNumJobs != 300);
	//uint32_t nNumJobsbef = pJob->State.load().nNumJobs;
	bool bR = pJob->State.compare_exchange_weak(Old, New);
	// if(bR)
	// {
	// 	printf("nNumJobs %d %d\n", nNumJobsbef, pJob->State.load().nNumJobs);
	// }
	return bR;
}

#endif

inline JqPipeHandle JqPipeHandleNull()
{
	JqPipeHandle H;
	H.Handle = 0;
	return H;
}

struct JqPipeStats
{
	std::atomic<uint64_t> nJobsAdded;
	std::atomic<uint64_t> nJobsFinished;
	std::atomic<uint64_t> nSubJobsAdded;
	std::atomic<uint64_t> nSubJobsFinished;

};

typedef void (*JqPipeRunJob)(JqPipeHandle Handle, uint32_t nExternalId, uint16_t nSubJob, int nNumJobs, int nRange);
typedef void (*JqPipeFinishJob)(JqPipeHandle Handle, uint32_t nExternalId, int nNumJobs);

struct JqPipe
{
	std::atomic<uint64_t>	nPut;
	std::atomic<uint64_t>	nGet;
	uint8_t					nPipeId;
	JqPipeJob				Jobs[JQ_PIPE_BUFFER_SIZE];

	JqPipeRunJob			StartJobFunc;
	JqPipeFinishJob			FinishJobFunc;
	JqPipeStats				Stats;
};


enum EJqPipeExecuteResult
{
	EJQ_EXECUTE_FAIL,
	EJQ_EXECUTE_SUCCES,
	EJQ_EXECUTE_FINISHED,
};

JQ_API JqPipeHandle 			JqPipeAdd(JqPipe* pPipe, uint32_t nExternalId, int nNumJobs, int nRange);
JQ_API EJqPipeExecuteResult		JqPipeExecute(JqPipe* pPipe, JqPipeHandle ExecuteHandle);
JQ_API bool 					JqPipeIsDone(JqPipe* pPipe, JqPipeHandle Job);
JQ_API bool 					JqPipeIsAllStarted(JqPipe* pPipe, JqPipeHandle nJob);
JQ_API void 					JqPipeDump(JqPipe* pPipe);


#ifdef JQ_PIPE_IMPL
#include <stdio.h>
int64_t JqTick();
int64_t JqTicksPerSecond();
std::atomic<JqJobState> State;

JqPipeHandle JqPipeAdd(JqPipe* pPipe, uint32_t nExternalId, int nNumJobs, int nRange)
{
	JQ_ASSERT(nNumJobs);
	if(nRange < 0)
	{
		nRange = nNumJobs;
	}
	JQ_ASSERT(nNumJobs > 0);
	//if(nNumJobs < 0)
	//{
	// 	nNumJobs = JqState.nNumWorkers;
	//}
	//JQ_ASSERT(nExternalId < (1<<12));
	uint64_t nNextHandle = 0;

	do
	{
		nNextHandle = pPipe->nPut.fetch_add(1);
		nNextHandle &= 0xffffffff;
		if(nNextHandle)
		{
			uint16_t nJobIndex = nNextHandle % JQ_PIPE_BUFFER_SIZE;
			JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];
			JqJobState State;
			do
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
						JqPipeHandle H;
						H.Handle = nNextHandle;
						H.Pad0 = 0;
						H.Pipe = pPipe->nPipeId;
						return H;
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
			JqPipeHandle Handle;
			Handle.HandleInt = nHandleInternal;
			Handle.Pad0 = 0;
			Handle.Pipe = pPipe->nPipeId;
			pPipe->FinishJobFunc(Handle, State.nExternalId, State.nNumJobs);
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
		// if(bIsLast)
		// {
		// 	uint64_t nGet = pPipe->nGet.load()+1;
		// 	if((0xffffffff & nGet) == 0)
		// 	{
		// 		nGet++;
		// 	}
		// 	pPipe->nGet.store(1);
		// }
		//execute.

		{
			//JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
			uint16_t nJobIndex = nHandleInternal % JQ_PIPE_BUFFER_SIZE;
			JqPipeJob* pJob = &pPipe->Jobs[nJobIndex];

			JqJobState State = JqJobStateLoad(pJob);
			JqPipeHandle Handle;			
			JQ_ASSERT(State.nNumFinished < State.nNumJobs);
			Handle.HandleInt = nHandleInternal;
			Handle.Pad0 = 0;
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
//			uint32_t nHandle = (uint32_t)nGet;
			JqPipeHandle H;
			H.HandleInt = nGet & 0xffffffff;
			H.Pad0 = 0;
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
	return JQ_LE_WRAP_SHIFT(nHandleInternal, nFinished, 32) || State.nNumStarted == State.nNumJobs;
}

#include <stdio.h>

void JqPipeDump(JqPipe* pPipe)
{
	printf("***** PIPE DUMP BEGIN %d\n", pPipe->nPipeId);
	printf(" Put %lld\n", pPipe->nPut.load());
	printf(" Get %lld\n", pPipe->nGet.load());
	for(uint32_t i = 0; i < JQ_PIPE_BUFFER_SIZE; ++i)
	{
		JqPipeJob* pJob = &pPipe->Jobs[i];
		JqJobState State = JqJobStateLoad(pJob);

		if(State.nStartedHandle != State.nFinishedHandle ||
			State.nNumStarted != State.nNumJobs || 
			State.nNumFinished != State.nNumJobs)
		{
			printf(" JOB %04d :: ext %d (%3d/%3d/%3d) range %d, (%d/%d)\n", 
				i, State.nExternalId,
				State.nNumStarted,
				State.nNumFinished,
				State.nNumJobs,
				State.nRange,
				State.nStartedHandle,
				State.nFinishedHandle);
		}
	}

	printf("***** PIPE DUMP END   %d\n", pPipe->nPipeId);


}
#endif
