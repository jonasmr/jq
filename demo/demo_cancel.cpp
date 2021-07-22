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

#define JQ_CANCEL_TEST 1
int64_t JqTick();
int64_t JqTicksPerSecond();

uint32_t g_Reset = 0;

#include <thread>

#include "../jq.h"
#include "../jqnode.h"

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
	if(g_DontSleep)
	{
		return 0;
	}
	uint32_t result			 = 0;
	uint64_t nTick			 = JqTick();
	uint64_t nTicksPerSecond = JqTicksPerSecond();
	do
	{
		for(uint32_t i = 0; i < 1000; ++i)
		{
			result |= i << (i & 7); // do something.. whatever
		}
	} while((1000000ull * (JqTick() - nTick)) / nTicksPerSecond < nUs);
	return result;
}

void JobTree2(int nStart)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree2", 0xff);
	JobSpinWork(5 + rand() % 100);
	g_nJobCount2.fetch_add(1);
}

void JobTree1()
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree1", 0xff0000);
	if(g_FewJobs)
	{
		JqAdd(JobTree2, 2, JOB_COUNT_2);
		g_nExternalStats++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_2; ++i)
		{
			JqAdd(JobTree2, 2, 1);
			g_nExternalStats++;
		}
	}
	JobSpinWork(50 + rand() % 100);
	g_nJobCount1.fetch_add(1);
	// printf("jc1 %d :: %d\n", njc1, JOB_COUNT_1);
}

void JobTree0(void* pArg, int nStart, int nEnd)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree0", 0x00ff00);
	if(g_FewJobs)
	{
		JqAdd(JobTree1, 2, JOB_COUNT_1);
		g_nExternalStats++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_1; ++i)
		{
			JqAdd(JobTree1, 2, 1);
			g_nExternalStats++;
		}
	}
	JobSpinWork(50 + rand() % 100);
	((int*)pArg)[nStart] = 1;
	g_nJobCount0.fetch_add(1);
	// printf("jc0 %d :: %d\n", njc0, JOB_COUNT_0);
}

void JobTree()
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree", 0xff5555);
	JobSpinWork(100);
	int		 lala[3]   = { 0, 0, 0 };
	uint64_t nJobTree0 = JqAdd([&](int s, int e) { JobTree0((void*)&lala[0], s, e); }, 2, 3);
	g_nExternalStats++;
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree Wait", 0xff5555);
	JqWait(nJobTree0);
	DEMO_ASSERT(lala[0] == 1);
	DEMO_ASSERT(lala[1] == 1);
	DEMO_ASSERT(lala[2] == 1);

	g_nJobCount.fetch_add(1);
	// printf("jc0 %d :: %d\n", njc, JOB_COUNT);
}

void JqRangeTest(void* pArray, int nBegin, int nEnd)
{
	int* pIntArray = (int*)pArray;
	for(int i = nBegin; i < nEnd; ++i)
		pIntArray[i] = 1;
}

#ifdef _WIN32
void uprintf(const char* fmt, ...);
#else
#define uprintf printf
#endif
extern uint32_t g_TESTID;

void JqTestPrio()
{
	g_DontSleep = 0;
	uint64_t J1 = JqAdd(
		[](int b, int e) {
			MICROPROFILE_SCOPEI("JQ_TEST", "P7", 0xffffff);
			JobSpinWork(5000);
		},
		7, 3);
	uint64_t J2 = JqAdd(
		[](int b, int e) {
			MICROPROFILE_SCOPEI("JQ_TEST", "P0", 0xff0000);
			JqAdd(
				[](int b, int e) {
					MICROPROFILE_SCOPEI("JQ_TEST", "P1", 0x0000ff);
					JqAdd([](int b, int e) { MICROPROFILE_SCOPEI("JQ_TEST", "P5", 0xffff00); }, 5, 500);
					JobSpinWork(20);
				},
				1, 20);
			JobSpinWork(2000);
		},
		0, 2, 1);

	JqWait(J1, JQ_WAITFLAG_EXECUTE_ANY | JQ_WAITFLAG_SLEEP);
	JqWait(J2);
}

struct SCancelJobState
{
	uint64_t		 nHandle;
	std::atomic<int> nStarted;
	std::atomic<int> nFinished;
	int				 nCancelled;
	int				 nCancelRequested;
};
std::atomic<int> g_CancelFinished;

void JqTestCancel()
{
	int				 nNumWorkers = JqGetNumWorkers();
	int				 nNumJobs	 = nNumWorkers * 2;
	SCancelJobState* Cancel		 = new SCancelJobState[nNumJobs];
	g_CancelFinished			 = 0;
	// start a bunch of jobs, cancel half of them
	uint64_t nGroup = JqGroupBegin(0);
	for(int i = 0; i < nNumJobs; ++i)
	{
		SCancelJobState* pState	   = &Cancel[i];
		Cancel[i].nStarted		   = 0;
		Cancel[i].nFinished		   = 0;
		Cancel[i].nCancelled	   = 0;
		Cancel[i].nCancelRequested = 0;
		Cancel[i].nHandle		   = JqAdd(
			 [pState] {
				 MICROPROFILE_SCOPEI("CANCEL", "WORK", MP_CYAN);
				 pState->nStarted.fetch_add(1);
				 JobSpinWork(5000);
				 pState->nFinished.fetch_add(1);
				 g_CancelFinished.fetch_add(1);
			 },
			 0);
	}
	JqGroupEnd();
	int nNumCancelled = 0;
	{
		MICROPROFILE_SCOPEI("CANCEL", "CANCEL", MP_CYAN);
		JobSpinWork(2000);
		if(1)
		{
			for(int i = 0; i < nNumWorkers; ++i)
			{
				do
				{
					int idx = rand() % nNumJobs;
					// int idx =
					// idx = nNumJobs-1;
					SCancelJobState* pState = &Cancel[idx];
					if(!pState->nCancelRequested)
					{
						if(JqCancel(pState->nHandle))
						{
							pState->nCancelled = 1;
							nNumCancelled++;
						}
						pState->nCancelRequested = 1;
						break;
					}
				} while(1);
			}
		}
	}

	JqWait(nGroup);
	for(int i = 0; i < nNumJobs; ++i)
	{
		SCancelJobState* pState = &Cancel[i];
		if(pState->nCancelled)
		{
			DEMO_ASSERT(pState->nStarted.load() == 0);
			DEMO_ASSERT(pState->nFinished.load() == 0);
		}
		else
		{
			DEMO_ASSERT(pState->nStarted.load() == 1);
			DEMO_ASSERT(pState->nFinished.load() == 1);
		}
	}
	printf("num cancelled %d/%d\n", nNumCancelled, nNumJobs);
}

// void JqTest()
// {
// 	static int frames = 0;
// 	static float fLimit = 5;
// 	static JqStats Stats ;
// 	static bool bFirst = true;
// 	bool bIncremental = true;
// 	static uint64_t TickLast = JqTick();

// 	if(bFirst || !bIncremental || g_Reset)
// 	{
// 		g_Reset = false;
// 		bFirst = false;
// 		memset(&Stats, 0, sizeof(Stats));
// 		TickLast = JqTick();
// 		printf("\n");
// 	}

// 	if(frames > fLimit)
// 	{
// 		fLimit = 5;
// 		JqStats Stats0;
// 		JqConsumeStats(&Stats0);
// 		Stats.Add(Stats0);
// 		static bool bFirst = true;
// 		static uint64_t H = Stats.nNextHandle;
// 		uint64_t nHandleConsumption = Stats.nNextHandle - H;
// 		H = Stats.nNextHandle;

// 		bool bUseWrapping = true;
// 		if(bFirst)
// 		{
// 			bFirst = false;
// 			bUseWrapping = false;
// 		printf("\n|Per ms  %10s/%10s/%10s, %10s/%10s|%8s %8s %8s|Total %8s/%8s, %8s/%8s|%8s|%12s|%7s|%7s\n",
// 			"JobAdd", "JobFin","JobCancel",
// 			"SubAdd", "SubFin",
// 			"Locks", "Waits", "Kicks",
// 			"JobAdd", "JobFin", "SubAdd", "SubFin","Handles", "WrapTime", "Time", "Workers");
// 		}

// 		uint64_t nDelta = JqTick() - TickLast;
// 		uint64_t nTicksPerSecond = JqTicksPerSecond();
// 		float fTime = 1000.f * nDelta / nTicksPerSecond;
// 		double HandlesPerMs = nHandleConsumption / fTime;
// 		double HandlesPerYear = (0x8000000000000000 / (365llu * 24 * 60 * 60 * 60 * 1000)) / HandlesPerMs;

// 		double WrapTime = (uint64_t)0x8000000000000000 / (nHandleConsumption?nHandleConsumption:1) * (1.0 / (365*60.0* 60.0 * 60.0 * 24.0));
// 		(void)WrapTime;
// 		printf("%c|        %10.2f/%10.2f/%10.2f, %10.2f/%10.2f|%8.2f %8.2f %8.2f|      %8d/%8d, %8d/%8d|%8lld|%12.2f|%6.2fs|%2d,%c,%c     ",
// 			bUseWrapping ? '\r' : ' ',
// 			Stats.nNumAdded / (float)fTime,
// 			Stats.nNumFinished / (float)fTime,
// 			Stats.nNumCancelled / (float)fTime,
// 			Stats.nNumAddedSub / (float)fTime,
// 			Stats.nNumFinishedSub / (float)fTime,
// 			Stats.nNumLocks / (float)fTime,
// 			Stats.nNumWaitCond / (float)fTime,
// 			Stats.nNumWaitKicks / (float)fTime,
// 			Stats.nNumAdded,
// 			Stats.nNumFinished,
// 			Stats.nNumAddedSub,
// 			Stats.nNumFinishedSub,
// 			nHandleConsumption,
// 			HandlesPerYear,
// 			fTime / 1000.f,
// 			JqGetNumWorkers(),
// 			g_DontSleep?'d':' ',
// 			g_FewJobs?'f':' '

// 			);
// 		fflush(stdout);

// 		frames = 0;
// 		if(!bIncremental)
// 		{
// 			TickLast = JqTick();
// 		}

// 		if (fTime / 1000.f > 60.f)
// 		{
// 			g_nNumWorkers++;
// 			g_Reset = true;
// 		}
// 	}

// 	++frames;
// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST_WAIT_ALL", 0xff00ff);
// 		JqWaitAll();
// 	}
// 	MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST", 0xff00ff);

// 	g_nLowCount = 0;

// 	uint64_t nJob = JqAdd( [](int begin, int end)
// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "JobLow", 0x0000ff);
// 		g_nLowCount++;
// 	}, 3, JOB_COUNT_LOW);
// 	g_nExternalStats ++;
// 	JqWait(nJob);
// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
// 		JobSpinWork(500);
// 	}
// 	uint64_t nStart = JqTick();
// 	#if 1
// 	while((JqTick() - nStart) * 1000.f / JqTicksPerSecond() < 14)
// 	{
// 		g_nJobCount = 0;
// 		g_nJobCount0 = 0;
// 		g_nJobCount1 = 0;
// 		g_nJobCount2 = 0;

// 		uint64_t nJobMedium = JqAdd(JobTree, 0, JOB_COUNT);
// 		g_nExternalStats ++;
// 		{
// 			MICROPROFILE_SCOPEI("JQDEMO", "JqWaitMedium", 0xff0000);
// 			JqWait(nJobMedium);

// 		}

// 		DEMO_ASSERT(g_nJobCount == JOB_COUNT);
// 		DEMO_ASSERT(g_nJobCount0 == JOB_COUNT_0 * JOB_COUNT);
// 		DEMO_ASSERT(g_nJobCount1 == JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
// 		DEMO_ASSERT(g_nJobCount2 == JOB_COUNT_2 * JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
// 	}
// 	#endif
// }

// #define JQ_NODE_TEST 0

#define JQ_TEST_WORKERS 5

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

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif
	static JqAttributes Attr;
	JqInitAttributes(&Attr, JQ_TEST_WORKERS, JQ_TEST_WORKERS);
	Attr.Flags		  = nJqInitFlags;
	Attr.PipeOrder[0] = JqPipeOrder{ 7, { 0, 1, 2, 3, 4, 5, 6, 0xff } };
	Attr.PipeOrder[1] = JqPipeOrder{ 3, { 3, 2, 1, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.PipeOrder[2] = JqPipeOrder{ 2, { 5, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.PipeOrder[3] = JqPipeOrder{ 2, { 1, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.PipeOrder[4] = JqPipeOrder{ 1,
									 {
										 7,
										 0xff,
										 0xff,
										 0xff,
										 0xff,
										 0xff,
										 0xff,
										 0xff,
									 } };

	for(uint32_t i = 0; i < JQ_TEST_WORKERS; ++i)
	{
		Attr.WorkerOrderIndex[i] = i;
	}

	JqStart(&Attr);
	JqPipeOrder MyPipeConfig = JqPipeOrder{ 2, { 0, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	JqSetThreadPipeOrder(&MyPipeConfig);
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

		// #if JQ_STRESS_TEST
		// 		if(g_nNumWorkers != nNumWorkers)
		// 		{
		// 			nNumWorkers = g_nNumWorkers;
		// 			printf("\n");
		// 			JqStop();
		// 			JqStart(1 + nNumWorkers % 12, 0, nullptr);
		// 		}
		// #endif
		MicroProfileFlip(0);
		JqTestCancel();
	}
#ifdef _WIN32
	foo.join();
#endif
	JqStop();
	return 0;
}
