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

#include "jq.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>

#include <atomic>

#include "microprofile.h"

uint32_t JobSpinWork(uint32_t nUs)
{
	uint32_t result			 = 0;
	uint64_t nTick			 = JqGetTick();
	uint64_t nTicksPerSecond = JqGetTicksPerSecond();
	do
	{
		for(uint32_t i = 0; i < 1000; ++i)
		{
			result |= i << (i & 7); // do something.. whatever
		}
	} while((1000000ull * (JqGetTick() - nTick)) / nTicksPerSecond < nUs);
	return result;
}

#ifdef _WIN32
void uprintf(const char* fmt, ...);
#else
#define uprintf printf
#endif
extern uint32_t g_TESTID;

#define JQ_TEST_MAX_WORKERS 64

#define STAGES (32 * 8)
#define STAGE1_SPIN 100
#define STAGE2_SPIN 50

template <typename T>
T Min(const T& l, const T& r)
{
	return l < r ? l : r;
}

template <typename T>
T Max(const T& l, const T& r)
{
	return l > r ? l : r;
}

struct SBenchmarkData
{
	uint64_t TickStart;
	uint64_t TickEnd;
	uint64_t ThreadId;

	uint8_t padding[256 - 8 * 3];
};

SBenchmarkData g_Stage1[STAGES];
SBenchmarkData g_Stage2[STAGES];
SBenchmarkData g_StageFinal;

void Stage1(int Index)
{
	SBenchmarkData& Data = g_Stage1[Index];
	Data.TickStart		 = JqGetTick();
	JobSpinWork(STAGE1_SPIN);
	Data.TickEnd = JqGetTick();
}
void Stage2(int Index)
{

	SBenchmarkData& Data = g_Stage2[Index];
	Data.TickStart		 = JqGetTick();
	JobSpinWork(STAGE2_SPIN);
	Data.TickEnd = JqGetTick();
}

void JobBenchmark(const uint32_t NumWorkers)
{
	MICROPROFILE_SCOPEI("JobBenchmark", "JobBenchmark", MP_AUTO);
	memset(g_Stage1, 0, sizeof(g_Stage1));
	memset(g_Stage2, 0, sizeof(g_Stage2));
	memset(&g_StageFinal, 0, sizeof(g_StageFinal));

	uint64_t TicksPerSecond = JqGetTicksPerSecond();

	// run it 100 times
	struct TimeInfo
	{
		double Time			= 0;
		double Core			= 0;
		double Spin			= 0;
		double CoreOverhead = 0;
		double First		= 0;
		double Last			= 0;
	};
	TimeInfo TimeTotal;
	TimeInfo TimeMin;
	TimeInfo TimeMax;
	TimeMax.Time		 = 0.0;
	TimeMax.Core		 = 0.0;
	TimeMax.Spin		 = 0.0;
	TimeMax.CoreOverhead = 0.0;
	TimeMax.First		 = 0.0;
	TimeMax.Last		 = 0.0;

	TimeMin.Time		 = 1e38;
	TimeMin.Core		 = 1e38;
	TimeMin.Spin		 = 1e38;
	TimeMin.CoreOverhead = 1e38;
	TimeMin.First		 = 1e38;
	TimeMin.Last		 = 1e38;

	const uint32_t RepeatCount = 256;
	for(uint32_t i = 0; i < RepeatCount; ++i)
	{
		uint64_t Start = JqGetTick();

		JqHandle Stage1Job = JqAdd(
			"Stage1Job",
			[](int Index) {
				Stage1(Index);
			},
			0, STAGES);

		JqWait(Stage1Job, JQ_WAITFLAG_BLOCK);

		uint64_t End = JqGetTick();

		uint64_t TotalTicks		= End - Start;
		uint64_t TotalCoreTicks = TotalTicks * (NumWorkers);
		uint64_t JobTime		= 0;

		uint64_t FirstJob = (uint64_t)-1;
		uint64_t LastJob  = 0;
		for(uint32_t i = 0; i < STAGES; ++i)
		{
			if(g_Stage1[i].TickStart)
			{
				JobTime += g_Stage1[i].TickEnd - g_Stage1[i].TickStart;
				LastJob	 = g_Stage1[i].TickStart > LastJob ? g_Stage1[i].TickStart : LastJob;
				FirstJob = g_Stage1[i].TickStart < FirstJob ? g_Stage1[i].TickStart : FirstJob;
			}
		}

		double Time			= 1000.0 * TotalTicks / TicksPerSecond;
		double Core			= 1000.0 * TotalCoreTicks / TicksPerSecond;
		double Spin			= 1000.0 * JobTime / TicksPerSecond;
		double CoreOverhead = Core - Spin;
		double First		= 1000.0 * (FirstJob - Start) / TicksPerSecond;
		double Last			= 1000.0 * (LastJob - Start) / TicksPerSecond;

		TimeTotal.Time += Time;
		TimeTotal.Core += Core;
		TimeTotal.Spin += Spin;
		TimeTotal.CoreOverhead += CoreOverhead;
		TimeTotal.First += First;
		TimeTotal.Last += Last;

		TimeMin.Time		 = Min(Time, TimeMin.Time);
		TimeMin.Core		 = Min(Core, TimeMin.Core);
		TimeMin.Spin		 = Min(Spin, TimeMin.Spin);
		TimeMin.CoreOverhead = Min(CoreOverhead, TimeMin.CoreOverhead);
		TimeMin.First		 = Min(First, TimeMin.First);
		TimeMin.Last		 = Min(Time, TimeMin.Last);

		TimeMax.Time		 = Max(Time, TimeMax.Time);
		TimeMax.Core		 = Max(Core, TimeMax.Core);
		TimeMax.Spin		 = Max(Spin, TimeMax.Spin);
		TimeMax.CoreOverhead = Max(CoreOverhead, TimeMax.CoreOverhead);
		TimeMax.First		 = Max(First, TimeMax.First);
		TimeMax.Last		 = Max(Time, TimeMax.Last);
	}
	TimeTotal.Time /= double(RepeatCount);
	TimeTotal.Core /= double(RepeatCount);
	TimeTotal.Spin /= double(RepeatCount);
	TimeTotal.CoreOverhead /= double(RepeatCount);
	TimeTotal.First /= double(RepeatCount);
	TimeTotal.Last /= double(RepeatCount);

	if(NumWorkers == 1)
	{

		printf("Cpus | Time                           | CoreTime                       | TimeFirst                      | TimeLast                       |          | \n");
		printf("     |      Avg        Min        Max |      Avg        Min        Max |      Avg        Min        Max |      Avg        Min        Max | Ovh Avg  | SpinAvg   \n");
		printf("-----|--------------------------------|--------------------------------|--------------------------------|--------------------------------|----------|---------- \n");
	}

	printf("%4d | %8.2f / %8.2f / %8.2f | %8.2f / %8.2f / %8.2f | %8.2f / %8.2f / %8.2f | %8.2f / %8.2f / %8.2f | %8.2f | %8.2f\n", NumWorkers, TimeTotal.Time, TimeMin.Time, TimeMax.Time,
		   TimeTotal.Core, TimeMin.Core, TimeMax.Core, TimeTotal.First, TimeMin.First, TimeMax.First, TimeTotal.Last, TimeMin.Last, TimeMax.Last, TimeTotal.CoreOverhead, TimeTotal.Spin);
}

int main(int argc, char* argv[])
{

	uint32_t nJqInitFlags = JQ_INIT_USE_SEPERATE_STACK;
	for(int i = 1; i < argc; ++i)
	{
		if(0 == strcmp("-ns", argv[i]))
		{
			printf("disabling seperate stack\n");
			nJqInitFlags &= ~JQ_INIT_USE_SEPERATE_STACK;
		}
	}
	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif

	uint32_t MaxWorkers = JqGetNumCpus();

	for(uint32_t i = 1; i < MaxWorkers; ++i)
	{
		const uint32_t NumWorkers = i;
		JqAttributes   Attr;
		uint64_t	   Affinity = 0;
		for(uint64_t i = 0; i < NumWorkers; ++i)
		{
			Affinity |= (1 << i);
		}
		JqInitAttributes(&Attr, 1, NumWorkers);
		Attr.Flags		   = nJqInitFlags;
		Attr.QueueOrder[0] = JqQueueOrder{ 7, { 0, 1, 2, 3, 4, 5, 6, 0xff } };
		for(uint32_t i = 0; i < NumWorkers; ++i)
		{
			Attr.WorkerOrderIndex[i]	 = 0;
			Attr.WorkerThreadAffinity[i] = Affinity;
		}
		JqStart(&Attr);
		JqSetThreadQueueOrder(&Attr.QueueOrder[0]);

		JobBenchmark(i);

		JqStop();
	}
	return 0;
}
