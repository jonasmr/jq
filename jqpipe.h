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

// Ultra simple lockless job pipe

#include "jq.h"
#include "jqinternal.h"

#include <stddef.h>
#include <stdint.h>
#include <atomic>
#ifndef _WIN32
#include <pthread.h>
#endif

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
			uint64_t Invalid	: 24;
			uint64_t Pipe 		: 8;
		};
	};
};
static_assert(sizeof(JqJobState) == 16, "invalid size");
static_assert(sizeof(JqPipeHandle) == 8, "invalid size");

#include <atomic>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_ATOMIC
#endif

#ifdef WIN32_ATOMIC
#define JQ_PIPE_ALIGN_16 __declspec(align(16))
#else
#define JQ_PIPE_ALIGN_16 
#endif
#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable:4324)
#endif


struct JQ_PIPE_ALIGN_16 JqPipeJob
{
	friend JqJobState JqJobStateLoad(JqPipeJob* pJob);
	friend bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old);
private:
#ifdef WIN32_ATOMIC
	volatile __int64 StateArray[2];
#else
	std::atomic<JqJobState> State;
#endif
};

#ifdef WIN32_ATOMIC
#include <stdio.h>
#include <intrin.h>
inline 
JqJobState JqJobStateLoad(JqPipeJob* pJob)
{
	JqJobState R;
	R.Atomic[0] = pJob->StateArray[0];
	R.Atomic[1] = pJob->StateArray[1];
	return R;
}
inline 
bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old)
{
	bool bR = 1 == InterlockedCompareExchange128((int64_t*)&pJob->StateArray[0], (int64_t)New.Atomic[1], (int64_t)New.Atomic[0], (int64_t*)&Old.Atomic[0]);
	return bR;
}
#else
inline 
JqJobState JqJobStateLoad(JqPipeJob* pJob)
{
	return pJob->State.load(std::memory_order_relaxed);
}
inline
bool JqJobStateCompareAndSwap(JqPipeJob* pJob, JqJobState& New, JqJobState& Old)
{
	bool bR = pJob->State.compare_exchange_weak(Old, New);
	return bR;
}
#endif
inline JqPipeHandle JqPipeHandleNull()
{
	JqPipeHandle H;
	H.Handle = 0;
	return H;
}
inline JqPipeHandle JqPipeHandleInvalid()
{
	JqPipeHandle H;
	H.Handle = 0;
	H.Invalid = 1;
	H.Pipe = 255;
	return H;
}

struct JqPipeStats
{
	std::atomic<uint64_t> nJobsAdded;
	std::atomic<uint64_t> nJobsFinished;
	std::atomic<uint64_t> nSubJobsAdded;
	std::atomic<uint64_t> nSubJobsFinished;
	std::atomic<uint64_t> nMaxHandleRetries;

};

typedef void (*JqPipeRunJob)(JqPipeHandle Handle, uint32_t nExternalId, uint16_t nSubJob, int nNumJobs, int nRange);
typedef void (*JqPipeFinishJob)(JqPipeHandle Handle, uint32_t nExternalId, int nNumJobs, int nCancel);

struct JqPipe
{
	std::atomic<uint64_t>	nPut;
	std::atomic<uint64_t>	nGet;
	std::atomic<uint64_t>	nHandle;
	uint8_t					nPipeId;
	JqPipeJob				Jobs[JQ_PIPE_BUFFER_SIZE];

	JqPipeRunJob			StartJobFunc;
	JqPipeFinishJob			FinishJobFunc;
	JqPipeStats				Stats;
};

#ifdef _WIN32
#pragma warning(pop)
#endif

enum EJqPipeExecuteResult
{
	EJQ_EXECUTE_FAIL,
	EJQ_EXECUTE_SUCCES,
	EJQ_EXECUTE_FINISHED,
};

JQ_API JqPipeHandle 			JqPipeAdd(JqPipe* pPipe, uint32_t nExternalId, int nNumJobs, int nRange);
JQ_API bool 					JqPipeCancel(JqPipe* pPipe, JqPipeHandle PipeHandle);

JQ_API EJqPipeExecuteResult		JqPipeExecute(JqPipe* pPipe, JqPipeHandle ExecuteHandle);
JQ_API bool 					JqPipeIsDone(JqPipe* pPipe, JqPipeHandle Job);
JQ_API bool 					JqPipeIsAllStarted(JqPipe* pPipe, JqPipeHandle nJob);
JQ_API void 					JqPipeDump(FILE* F, JqPipe* pPipe);
JQ_API void JqDump();

