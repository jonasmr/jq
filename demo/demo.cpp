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

#include <stdio.h>
#include <stdarg.h>
#include <string>
#include <stdlib.h>
#ifdef _WIN32
#include <Windows.h>
#include <conio.h>
#else
#include <curses.h>
#endif

#include "microprofile.h"





#define WIDTH 1024
#define HEIGHT 600
uint32_t g_FewJobs = 1;
uint32_t g_DontSleep = 1;

uint32_t g_nNumWorkers = 1;
uint32_t g_nQuit = 0;
uint32_t g_MouseX = 0;
uint32_t g_MouseY = 0;
uint32_t g_MouseDown0 = 0;
uint32_t g_MouseDown1 = 0;
int g_MouseDelta = 0;

MICROPROFILE_DEFINE(MAIN, "MAIN", "Main", 0xff0000);
#ifdef _WIN32
#define DEMO_ASSERT(a) do{if(!(a)){__debugbreak();} }while(0)
#else
#define DEMO_ASSERT(a) do{if(!(a)){__builtin_trap();} }while(0)
#endif

#define JQ_STRESS_TEST 1
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
	uint32_t result = 0;
	uint64_t nTick = JqTick();
	uint64_t nTicksPerSecond = JqTicksPerSecond();
	do
	{	
		for(uint32_t i = 0; i < 1000; ++i)
		{
			result |= i << (i&7); //do something.. whatever
		}	
	}while( (1000000ull*(JqTick()-nTick)) / nTicksPerSecond < nUs);
	return result;

}

void JobTree2(int nStart)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree2", 0xff);
	JobSpinWork(5+ rand() % 100);
	g_nJobCount2.fetch_add(1);
}


void JobTree1()
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree1", 0xff0000);
	if(g_FewJobs)
	{
		JqAdd(JobTree2, 2, JOB_COUNT_2);
		g_nExternalStats ++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_2; ++i)
		{
			JqAdd(JobTree2, 2, 1);
			g_nExternalStats ++;
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
		g_nExternalStats ++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_1; ++i)
		{
			JqAdd(JobTree1, 2, 1);
			g_nExternalStats ++;
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
	int lala[3]={0,0,0};
	uint64_t nJobTree0 = JqAdd(
		[&](int s, int e)
		{
			JobTree0((void*)&lala[0],s,e);
		}, 2, 3);
	g_nExternalStats ++;
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
	g_DontSleep =  0;
	uint64_t J1 = JqAdd([](int b, int e)
	{
		MICROPROFILE_SCOPEI("JQ_TEST", "P7", 0xffffff);
		JobSpinWork(5000);
	}, 7, 3);
	uint64_t J2 = JqAdd([](int b, int e)
	{
		MICROPROFILE_SCOPEI("JQ_TEST", "P0", 0xff0000);		
		JqAdd([](int b, int e)
		{
			MICROPROFILE_SCOPEI("JQ_TEST", "P1", 0x0000ff);
			JqAdd([](int b, int e)
			{
				MICROPROFILE_SCOPEI("JQ_TEST", "P5", 0xffff00);

			}, 5, 500);
			JobSpinWork(20);

		}, 1, 20);
		JobSpinWork(2000);

	}, 0, 2, 1);

	JqWait(J1, JQ_WAITFLAG_EXECUTE_ANY| JQ_WAITFLAG_SLEEP);
	JqWait(J2);

}
void JqTest()
{
	static int frames = 0;
	static float fLimit = 5;
	static JqStats Stats ;
	static bool bFirst = true;
	bool bIncremental = true;
	static uint64_t TickLast = JqTick();

	if(bFirst || !bIncremental || g_Reset)
	{
		g_Reset = false;
		bFirst = false;
		memset(&Stats, 0, sizeof(Stats));
		TickLast = JqTick();
		printf("\n");
	}

	if(frames > fLimit)
	{
		fLimit = 5;
		JqStats Stats0;
		JqConsumeStats(&Stats0);
		Stats.Add(Stats0);
		static bool bFirst = true;		
		static uint64_t H = Stats.nNextHandle;
		uint64_t nHandleConsumption = Stats.nNextHandle - H;
		H = Stats.nNextHandle;

		bool bUseWrapping = true;
		if(bFirst)
		{
			bFirst = false;
			bUseWrapping = false;
		printf("\n|Per ms  %10s/%10s, %10s/%10s|%8s %8s %8s|Total %8s/%8s, %8s/%8s|%8s|%12s|%7s|%7s\n", 
			"JobAdd", "JobFin",
			"SubAdd", "SubFin",
			"Locks", "Waits", "Kicks", 
			"JobAdd", "JobFin", "SubAdd", "SubFin","Handles", "WrapTime", "Time", "Workers");
		}

		uint64_t nDelta = JqTick() - TickLast;
		uint64_t nTicksPerSecond = JqTicksPerSecond();
		float fTime = 1000.f * nDelta / nTicksPerSecond;
		double HandlesPerMs = nHandleConsumption / fTime;
		double HandlesPerYear = (0x8000000000000000 / (365llu * 24 * 60 * 60 * 60 * 1000)) / HandlesPerMs;


		double WrapTime = (uint64_t)0x8000000000000000 / (nHandleConsumption?nHandleConsumption:1) * (1.0 / (365*60.0* 60.0 * 60.0 * 24.0));
		(void)WrapTime;
		printf("%c|        %10.2f/%10.2f, %10.2f/%10.2f|%8.2f %8.2f %8.2f|      %8d/%8d, %8d/%8d|%8lld|%12.2f|%6.2fs|%2d,%c,%c     ",
			bUseWrapping ? '\r' : ' ',
			Stats.nNumAdded / (float)fTime,
			Stats.nNumFinished / (float)fTime,
			Stats.nNumAddedSub / (float)fTime,
			Stats.nNumFinishedSub / (float)fTime,
			Stats.nNumLocks / (float)fTime,
			Stats.nNumWaitCond / (float)fTime,
			Stats.nNumWaitKicks / (float)fTime,
			Stats.nNumAdded,
			Stats.nNumFinished,
			Stats.nNumAddedSub,
			Stats.nNumFinishedSub,
			nHandleConsumption,
			HandlesPerYear,
			fTime / 1000.f,
			JqGetNumWorkers(),
			g_DontSleep?'d':' ',
			g_FewJobs?'f':' '

			); 
		fflush(stdout);

		frames = 0;
		if(!bIncremental)
		{
			TickLast = JqTick();
		}

		if (fTime / 1000.f > 60.f)
		{
			g_nNumWorkers++;
			g_Reset = true;
		}
	}

	++frames;
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST_WAIT_ALL", 0xff00ff);
		JqWaitAll();
	}
	MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST", 0xff00ff);

	g_nLowCount = 0;


	uint64_t nJob = JqAdd( [](int begin, int end)
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JobLow", 0x0000ff);
		g_nLowCount++;
	}, 3, JOB_COUNT_LOW);
	g_nExternalStats ++;
	JqWait(nJob);
	{
		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
		JobSpinWork(500);
	}
	uint64_t nStart = JqTick();	
	#if 1
	while((JqTick() - nStart) * 1000.f / JqTicksPerSecond() < 14)
	{
		g_nJobCount = 0;
		g_nJobCount0 = 0;
		g_nJobCount1 = 0;
		g_nJobCount2 = 0;


		uint64_t nJobMedium = JqAdd(JobTree, 0, JOB_COUNT);
		g_nExternalStats ++;
		{
			MICROPROFILE_SCOPEI("JQDEMO", "JqWaitMedium", 0xff0000);
			JqWait(nJobMedium);

		}

		DEMO_ASSERT(g_nJobCount == JOB_COUNT);
		DEMO_ASSERT(g_nJobCount0 == JOB_COUNT_0 * JOB_COUNT);
		DEMO_ASSERT(g_nJobCount1 == JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
		DEMO_ASSERT(g_nJobCount2 == JOB_COUNT_2 * JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
	}
	#endif
}



#define JQ_NODE_TEST 0

#define JQ_TEST_WORKERS 5

int main(int argc, char* argv[])
{

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif
	//}
	static uint32_t nNumWorkers = g_nNumWorkers;
#if JQ_STRESS_TEST
	JqStart(nNumWorkers, 0, nullptr);
#else
	uint8_t nPipeConfig[JQ_NUM_PIPES * JQ_TEST_WORKERS] = 
	{
		0, 1, 2, 3,				4, 5, 6, 0xff,
		3, 2, 1, 0xff,			0xff, 0xff, 0xff, 0xff,
		5, 1, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,
		1, 5, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,
		7, 0xff, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,

	};
	//JQ_NUM_PIPES
	JqStart(JQ_TEST_WORKERS, sizeof(nPipeConfig), nPipeConfig);

	uint8_t MyPipeConfig[JQ_NUM_PIPES] =
	{
		0,5, 0xff, 0xff,	0xff, 0xff, 0xff, 0xff,
	};

	JqSetThreadPipeConfig(MyPipeConfig);
#endif
	//JqStartSentinel(20);
	//I feel like im gonna burn someday for this code, but its the only way i know how to write it.

#ifdef _WIN32
	std::atomic<int> keypressed;
	std::thread	foo(
		[&]()
	{
		int c = 0;
		while (!g_nQuit && c != 'q' && c != 27 )
		{
			c = _getch(); 
			keypressed.exchange(c);
		}
		g_nQuit = 1;
	});
#else

	#if 0 == JQ_NODE_TEST
	WINDOW* w = initscr();
	cbreak();
	nodelay(w, TRUE);
	atexit([]{endwin();});
	#endif
#endif


#if JQ_NODE_TEST
	JqNode A(
		[]
		{
			printf("NODE A %d-%d\n",0,0);
		}, 1, 3, 10);
	JqNode B(
		[](int b, int e)
		{
			printf("NODE B %d-%d\n",b,e);
		}, 1, 50);
	JqNode B1(
		[](int b, int e)
		{
			printf("NODE B1 %d-%d\n",b,e);
		}, 1, 2);
	JqNode C(
		[](int b, int e)
		{
			printf("NODE C %d-%d\n",b,e);
		}, 1, 5);
	JqNode D(
		[](int b)
		{
			printf("NODE D %d\n",b);
		}, 1, 10);
	JqNode X(
		[](int b, int e)
		{
			printf("NODE X %d-%d\n",b,e);
		}, 1, 1);

	B.After(A);
	B1.After(A);
	C.After(B1);
	D.After(B1);
	X.After(C,D,A,B1);

	A.Run();
	A.Wait();

	printf("XXX RUN 1 DONE\n");


	A.Reset();
	A.Run();
	A.Wait();
	printf("YYY RUN 2 DONE\n");
	g_nQuit = 1;
	//exit(0);
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
		if (key)
		{
			switch (key)
			{
			case 'v':
				g_Reset = 1; break;
			case 'x':
				g_FewJobs = !g_FewJobs; 
				g_Reset = 1; break;
				break;
			case 'c':
				g_DontSleep = !g_DontSleep;
				g_Reset = 1; break;
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

#if JQ_STRESS_TEST
		if(g_nNumWorkers != nNumWorkers)
		{
			nNumWorkers = g_nNumWorkers;
			printf("\n");
			JqStop();
			JqStart(1 + nNumWorkers % 12, 0, nullptr);
		}
#endif
		MicroProfileFlip(0);
		{
#if JQ_STRESS_TEST
			JqTest();
#else
			JqTestPrio();
#endif
		}

	}
#ifdef _WIN32 
	foo.join();
#endif
	JqStop();
	return 0;
}
