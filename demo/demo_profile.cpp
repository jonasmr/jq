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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <conio.h>
#else
#include <curses.h>
#endif

#include "microprofile.h"

#define WIDTH 1024
#define HEIGHT 600
uint32_t g_FewJobs	 = 1;
uint32_t g_DontSleep = 1;

uint32_t g_nNumWorkers = 1;
uint32_t g_nQuit	   = 0;
uint32_t g_MouseX	   = 0;
uint32_t g_MouseY	   = 0;
uint32_t g_MouseDown0  = 0;
uint32_t g_MouseDown1  = 0;
int		 g_MouseDelta  = 0;

MICROPROFILE_DEFINE(MAIN, "MAIN", "Main", 0xff0000);
#ifdef _WIN32
#define DEMO_ASSERT(a)                                                                                                                                                                                 \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		if(!(a))                                                                                                                                                                                       \
		{                                                                                                                                                                                              \
			__debugbreak();                                                                                                                                                                            \
		}                                                                                                                                                                                              \
	} while(0)
#else
#define DEMO_ASSERT(a)                                                                                                                                                                                 \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
		if(!(a))                                                                                                                                                                                       \
		{                                                                                                                                                                                              \
			__builtin_trap();                                                                                                                                                                          \
		}                                                                                                                                                                                              \
	} while(0)
#endif

#define JQ_STRESS_TEST 1
#define JQ_CANCEL_TEST 0

uint32_t g_Reset = 0;

#include <thread>

#include "../jq.h"
#include <string.h>

#include <atomic>
std::atomic<int> g_nJobCount;
std::atomic<int> g_nJobCount0;
std::atomic<int> g_nJobCount1;
std::atomic<int> g_nJobCount2;
std::atomic<int> g_nLowCount;
std::atomic<int> g_nExternalStats;

#define JOB_COUNT 1
#define JOB_COUNT_0 3
#define JOB_COUNT_1 3
#define JOB_COUNT_2 20
#define JOB_COUNT_LOW 300

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
uint32_t	   g_NumWorkers = JQ_TEST_MAX_WORKERS;

void Stage1(int Index)
{
	SBenchmarkData& Data = g_Stage1[Index];
	Data.TickStart		 = JqGetTick();

	MICROPROFILE_SCOPEI("JobBenchmark", "Stage1", MP_AUTO);

	JobSpinWork(STAGE1_SPIN);
	Data.TickEnd = JqGetTick();
}
void Stage2(int Index)
{
	MICROPROFILE_SCOPEI("JobBenchmark", "Stage2", MP_ORANGE);

	SBenchmarkData& Data = g_Stage2[Index];
	Data.TickStart		 = JqGetTick();
	JobSpinWork(STAGE2_SPIN);
}

void JobBenchmark()
{
	MICROPROFILE_SCOPEI("JobBenchmark", "JobBenchmark", MP_AUTO);
	memset(g_Stage1, 0, sizeof(g_Stage1));
	memset(g_Stage2, 0, sizeof(g_Stage2));
	memset(&g_StageFinal, 0, sizeof(g_StageFinal));

	uint64_t TicksPerSecond = JqGetTicksPerSecond();
	uint64_t Start			= JqGetTick();

	JqHandle Stage1Job = JqAdd([](int Index) { Stage1(Index); }, 0, STAGES);

	JqWait(Stage1Job);

	uint64_t End = JqGetTick();

	uint64_t TotalTicks		= End - Start;
	uint64_t TotalCoreTicks = TotalTicks * (g_NumWorkers);
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

	double Time				= 1000.0 * TotalTicks / TicksPerSecond;
	double TimeCore			= 1000.0 * TotalCoreTicks / TicksPerSecond;
	double TotalSpinTime	= 1000.0 * JobTime / TicksPerSecond;
	double TimeCoreOverhead = TimeCore - TotalSpinTime;
	double TimeFirstJob		= 1000.0 * (FirstJob - Start) / TicksPerSecond;
	double TimeLastJob		= 1000.0 * (LastJob - Start) / TicksPerSecond;

	printf("%2d Time %6.3fms Core Time %6.3fms \\ %6.3fms  (SpinTime %6.3fms) :: First Job: %6.3fms, Last Job %6.3fms\n", g_NumWorkers, Time, TimeCore, TimeCoreOverhead, TotalSpinTime, TimeFirstJob,
		   TimeLastJob);
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
		if(0 == memcmp("-cores=", argv[i], sizeof("-cores=") - 1))
		{
			char* pInteger = argv[i];
			pInteger += sizeof("-cores=") - 1;
			int ret;
			int r = sscanf(pInteger, "%d", &ret);
			if(r == 1)
			{
				if(g_NumWorkers > JQ_TEST_MAX_WORKERS)
				{
					JQ_BREAK();
				}
				g_NumWorkers = ret;
				printf("using %d worker threads (incl self)\n", g_NumWorkers);
			}
		}
	}

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif
	// static uint32_t nNumWorkers = g_nNumWorkers;
	static JqAttributes Attr;
	JqInitAttributes(&Attr, 1, g_NumWorkers - 1);
	Attr.Flags		   = nJqInitFlags;
	Attr.QueueOrder[0] = JqQueueOrder{ 7, { 0, 1, 2, 3, 4, 5, 6, 0xff } };
	for(uint32_t i = 0; i < g_NumWorkers - 1; ++i)
	{
		Attr.WorkerOrderIndex[i] = 0;
	}
	JqStart(&Attr);
	JqSetThreadQueueOrder(&Attr.QueueOrder[0]);

#ifdef _WIN32
	std::atomic<int> keypressed;
	std::thread		 foo([&]() {
		 int c = 0;
		 while(!g_nQuit && c != 'q' && c != 27)
		 {
			 c = _getch();
			 keypressed.exchange(c);
		 }
		 g_nQuit = 1;
	 });
#endif

	while(!g_nQuit)
	{
		MICROPROFILE_SCOPE(MAIN);

		int key;
#ifdef _WIN32
		key = keypressed.exchange(0);
#else
		key = getch();
#endif
		if(key)
		{
			switch(key)
			{
			case 'v':
				g_Reset = 1;
				break;
			case 'x':
				g_FewJobs = !g_FewJobs;
				g_Reset	  = 1;
				break;
				break;
			case 'c':
				g_DontSleep = !g_DontSleep;
				g_Reset		= 1;
				break;
				break;
			case ' ':
			{
				g_nNumWorkers++;
				g_Reset = 1;
			}
			break;
			case 'q':
				g_nQuit = 1;
				break;
			}
		}
		MicroProfileFlip(0);
		{
			JobBenchmark();
		}
	}
#ifdef _WIN32
	foo.join();
#endif
	JqStop();
	return 0;
}
