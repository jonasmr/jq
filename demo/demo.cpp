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
#include <SDL.h>
#include <string>


#include "microprofile.h"
#include "glinc.h"

#ifdef main
#undef main
#endif

#ifdef _WIN32
#undef near
#undef far
#define snprintf _snprintf
#include <windows.h>
#endif


#define WIDTH 1024
#define HEIGHT 600
uint32_t g_FewJobs = 0;
uint32_t g_DontSleep = 0;

uint32_t g_nNumWorkers = 1;
uint32_t g_nQuit = 0;
uint32_t g_MouseX = 0;
uint32_t g_MouseY = 0;
uint32_t g_MouseDown0 = 0;
uint32_t g_MouseDown1 = 0;
int g_MouseDelta = 0;

MICROPROFILE_DEFINE(MAIN, "MAIN", "Main", 0xff0000);

int64_t JqTick();
int64_t JqTicksPerSecond();

void HandleEvent(SDL_Event* pEvt)
{
	switch(pEvt->type)
	{
	case SDL_QUIT:
		g_nQuit = true;
		break;
	case SDL_KEYUP:
		if(pEvt->key.keysym.scancode == SDL_SCANCODE_ESCAPE)
		{
			g_nQuit = 1;
		}
		if(pEvt->key.keysym.scancode == SDL_SCANCODE_SPACE)
		{
			g_nNumWorkers ++;
		}

		if(pEvt->key.keysym.sym == 'x')
		{
			g_FewJobs = !g_FewJobs;
		}
		if(pEvt->key.keysym.sym == 'c')
		{
			g_DontSleep = !g_DontSleep;
		}

		if(pEvt->key.keysym.sym == 'z')
		{
			MicroProfileToggleDisplayMode();
		}
		if(pEvt->key.keysym.scancode == SDL_SCANCODE_RSHIFT)
		{
			MicroProfileTogglePause();
		}
		if(pEvt->key.keysym.scancode == SDL_SCANCODE_LCTRL)
		{
			MicroProfileModKey(0);
		}
		if(pEvt->key.keysym.sym == 'a')
		{
			MicroProfileDumpTimers();
		}
		break;
	case SDL_KEYDOWN:
		if(pEvt->key.keysym.scancode == SDL_SCANCODE_LCTRL)
		{
			MicroProfileModKey(1);
		}
		break;
	case SDL_MOUSEMOTION:
		g_MouseX = pEvt->motion.x;
		g_MouseY = pEvt->motion.y;
		break;
	case SDL_MOUSEBUTTONDOWN:
		if(pEvt->button.button == 1)
			g_MouseDown0 = 1;
		else if(pEvt->button.button == 3)
			g_MouseDown1 = 1;
		break;
	case SDL_MOUSEBUTTONUP:
		if(pEvt->button.button == 1)
		{
			g_MouseDown0 = 0;
		}
		else if(pEvt->button.button == 3)
		{
			g_MouseDown1 = 0;
		}
		break;
	case SDL_MOUSEWHEEL:
			g_MouseDelta -= pEvt->wheel.y;
		break;
	}
}



//comment out to test different modes
//#define JQ_NO_LAMBDA
//#define JQ_USE_STD_FUNCTION

#include "../jq.h"

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

//Helper macros to let the tests run with both interfaces.
#ifdef JQ_NO_LAMBDA
#define VOID_ARG void* pArg,
#define VOID_PARAM nullptr, 
#else
#define VOID_ARG
#define VOID_PARAM 
#endif


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

void JobTree2(VOID_ARG int nStart, int nEnd)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree2", 0xff);
	JobSpinWork(5+ rand() % 100);
	g_nJobCount2.fetch_add(1);
}


void JobTree1(VOID_ARG int nStart, int nEnd)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree1", 0xff0000);
	if(g_FewJobs)
	{
		JqAdd(JobTree2, 2, VOID_PARAM JOB_COUNT_2);
		g_nExternalStats ++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_2; ++i)
		{
			JqAdd(JobTree2, 2, VOID_PARAM 1);
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
		JqAdd(JobTree1, 2, VOID_PARAM JOB_COUNT_1);
		g_nExternalStats ++;
	}
	else
	{
		for(int i = 0; i < JOB_COUNT_1; ++i)
		{
			JqAdd(JobTree1, 2, VOID_PARAM 1);
			g_nExternalStats ++;
		}
	}
	JobSpinWork(50 + rand() % 100);
	((int*)pArg)[nStart] = 1;
	g_nJobCount0.fetch_add(1);
	// printf("jc0 %d :: %d\n", njc0, JOB_COUNT_0);

}

void JobTree(VOID_ARG int nStart, int nEnd)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree", 0xff5555);
	JobSpinWork(100);
	int lala[3]={0,0,0};
	#ifdef JQ_NO_LAMBDA
	uint64_t nJobTree0 = JqAdd(JobTree0, 2, (void*)&lala[0], 3);
	#else
	uint64_t nJobTree0 = JqAdd(
		[&](int s, int e)
		{
			JobTree0((void*)&lala[0],s,e);
		}, 2, 3);
	#endif
	g_nExternalStats ++;
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree Wait", 0xff5555);
	JqWait(nJobTree0);
	JQ_ASSERT(lala[0] == 1);
	JQ_ASSERT(lala[1] == 1);
	JQ_ASSERT(lala[2] == 1);

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

void JqTest()
{
	static int frames = 0;
	if(frames > 60)
	{
		JqStats Stats;
		JqConsumeStats(&Stats);
		static bool bFirst = true;		
		static uint64_t H = Stats.nNextHandle;
		static uint64_t TickLast = JqTick();
		uint64_t nHandleConsumption = Stats.nNextHandle - H;
		H = Stats.nNextHandle;

		bool bUseWrapping = true;
		if(bFirst)
		{
			bFirst = false;
			bUseWrapping = false;

		uprintf("|Per Sec %10s/%10s, %10s/%10s|%8s %8s %8s|Total %8s/%8s, %8s/%8s|%8s|%12s|\n", 
			"JobAdd", "JobFin",
			"SubAdd", "SubFin",
			"Locks", "Waits", "Kicks", 
			"JobAdd", "JobFin", "SubAdd", "SubFin","Handles", "WrapTime");
		}

		uint64_t nDelta = JqTick() - TickLast;
		uint64_t nTicksPerSecond = JqTicksPerSecond();
		float fTime = 1000.f * nDelta / nTicksPerSecond;


		double WrapTime = (uint64_t)0x8000000000000000 / (nHandleConsumption?nHandleConsumption:1) * (1.0 / (365*60.0* 60.0 * 60.0 * 24.0));
		uprintf("%c|        %10.2f/%10.2f, %10.2f/%10.2f|%8.2f %8.2f %8.2f|      %8d/%8d, %8d/%8d|%8lld|%12.2f|     ", 
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
			WrapTime
			); 
		fflush(stdout);


		frames = 0;
		TickLast = JqTick();
	}

	++frames;
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST_WAIT_ALL", 0xff00ff);
		JqWaitAll();
	}
	MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST", 0xff00ff);

	g_nLowCount = 0;


	uint64_t nJob = JqAdd( [](VOID_ARG int begin, int end)
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JobLow", 0x0000ff);
//		printf("run job %d\n", begin);
//		JQ_ASSERT(0);
	//	JobSpinWork(200);
		g_nLowCount++;
	}, 3, VOID_PARAM JOB_COUNT_LOW);
	g_nExternalStats ++;
	//printf("waiting\n");
	JqWait(nJob);
	//printf("done waiting\n");
	{
		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
		JobSpinWork(500);
	}

	//printf("job Count %d %d %d %d\n", g_nJobCount.load(), g_nJobCount0.load(), g_nJobCount1.load(), g_nJobCount2.load());

	uint64_t nStart = JqTick();	
	// float ft = (JqTick() - nStart) * 1000.f / JqTicksPerSecond();
	//printf("ft %f\n", ft);
	#if 1
	while((JqTick() - nStart) * 1000.f / JqTicksPerSecond() < 14)
	{
		g_nJobCount = 0;
		g_nJobCount0 = 0;
		g_nJobCount1 = 0;
		g_nJobCount2 = 0;


		uint64_t nJobMedium = JqAdd(JobTree, 0, VOID_PARAM JOB_COUNT);
		g_nExternalStats ++;
		//g_TESTID = (uint32_t)nJobMedium;
		// printf("job medium %lld\n", nJobMedium);
		{
			MICROPROFILE_SCOPEI("JQDEMO", "JqWaitMedium", 0xff0000);
			JqWait(nJobMedium);
			// printf("exp Count %d %d %d %d\n", JOB_COUNT,
			// JOB_COUNT_0 * JOB_COUNT,
			// JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT,
			// JOB_COUNT_2 * JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);

			//printf("job Count %d %d %d %d\n", g_nJobCount.load(), g_nJobCount0.load(), g_nJobCount1.load(), g_nJobCount2.load());
			//JqCrashAndDump();

		}

		JQ_ASSERT(g_nJobCount == JOB_COUNT);
		JQ_ASSERT(g_nJobCount0 == JOB_COUNT_0 * JOB_COUNT);
		JQ_ASSERT(g_nJobCount1 == JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
		JQ_ASSERT(g_nJobCount2 == JOB_COUNT_2 * JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
	}
	#endif

// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
// 		JobSpinWork(500);
// 	}


// 	{
// 		if(0)
// 		{
// 			MICROPROFILE_SCOPEI("JQDEMO", "JqWaitSpin", 0xff0000);
// 			JqWait(nJob, JQ_WAITFLAG_SLEEP|JQ_WAITFLAG_EXECUTE_ANY);
// 		}
// 		else
// 		{
// 			JqWait(nJob);
// 		}
// 	}

// 	JQ_ASSERT(g_nLowCount == JOB_COUNT_LOW);


// 	static int nNumJobs = 1;
// 	static int nRange = (4<<10)-5;

// 	int nData[4<<10] = {0};
// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "RangeTest", 0xff00ff);

// 		nNumJobs = (nNumJobs+1) % 21;
// 		if(nNumJobs == 0)
// 		{
// 			nNumJobs = 1;
// 			//printf("range test range %d\n", nRange);
// 			nRange = (nRange+1) % (4<<10);
// 		}
// 		for(int i = 0; i < nRange; ++i)
// 		{
// 			if(nData[i] != 0)
// 				JQ_BREAK();
// 		}
// #ifndef JQ_NO_LAMBDA
// 		uint64_t nRangeTest = JqAdd(
// 			[&](int nBegin, int nEnd)
// 		{
// 			for(int i = nBegin; i < nEnd; ++i)
// 			{
// 				nData[i] = 1;
// 			}
// 		}, 3, nNumJobs, nRange);
// #else
// 		uint64_t nRangeTest = JqAdd(
// 			[](void* pArray, int nBegin, int nEnd)
// 		{
// 			int* iArray = (int*)pArray;
// 			for(int i = nBegin; i < nEnd; ++i)
// 			{
// 				iArray[i] = 1;
// 			}
// 		}, 3, &nData[0], nNumJobs, nRange);
// #endif

// 		JqWait(nRangeTest);

// 		for(int i = 0; i < nRange; ++i)
// 		{
// 			if(nData[i] != 1)
// 				JQ_BREAK();
// 		}
// 	}
// 	{
// 		MICROPROFILE_SCOPEI("JQDEMO", "GroupTest", 0x66ff33);

// 		int lala[20] = {0};
// 		uint64_t nJobGroup = JqGroupBegin();

// #ifndef JQ_NO_LAMBDA
// 		for(int i = 0; i < 20; ++i)
// 		{
// 			JqAdd([i, &lala](int b, int e)
// 			{
// 				MICROPROFILE_SCOPEI("JQDEMO", "GroupJob", 0x33ff33);
// 				JobSpinWork(1000);
// 				lala[i] = 1;
// 			}, 7, 1);
// 		}
// #else
// 		for(int i = 0; i < 20; ++i)
// 		{
// 			JqAdd([](void* p, int b, int e)
// 			{
// 				MICROPROFILE_SCOPEI("JQDEMO", "GroupJob", 0x33ff00);

// 				JobSpinWork(1000);
// 				((int*)p)[0] = 1;
// 			}, 7, &lala[i], 1);
// 		}
// #endif
// 		JqGroupEnd();

// 		MICROPROFILE_SCOPEI("JQDEMO", "GroupWait", 0x77ff00);

// 		JqWait(nJobGroup);
// 		for(int i = 0; i < 20; ++i)
// 		{
// 			JQ_ASSERT(lala[i] == 1);
// 		}
// 	}
}



void MicroProfileQueryInitGL();
void MicroProfileDrawInit();
void MicroProfileBeginDraw(uint32_t nWidth, uint32_t nHeight, float* prj);
void MicroProfileEndDraw();


struct lala
{
	void* p1;
	void* p2;
};

std::atomic<lala> hest;
int main(int argc, char* argv[])
{

	std::atomic<lala> ged;
	lala l = {0, 0};
	ged.store(l);
	hest.compare_exchange_weak(l, l);

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");

	if(SDL_Init(SDL_INIT_VIDEO) < 0) {
		return 1;
	}
	

	//JqTest();
	static uint32_t nNumWorkers = g_nNumWorkers;
	JqStart(nNumWorkers);
	//JqStartSentinel(20);

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE,    	    8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,  	    8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,   	    8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,  	    8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  	    24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE,  	    8);	
	SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE,		    32);	
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,	    1);	
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetSwapInterval(1);

	SDL_Window * pWindow = SDL_CreateWindow("microprofiledemo", 10, 10, WIDTH, HEIGHT, SDL_WINDOW_OPENGL);
	if(!pWindow)
		return 1;

	SDL_GLContext glcontext = SDL_GL_CreateContext(pWindow);

	glewExperimental=1;
	GLenum err=glewInit();
	if(err!=GLEW_OK)
	{
		MP_BREAK();
	}
	glGetError(); //glew generates an error
		


	MicroProfileQueryInitGL();
	MicroProfileDrawInit();
	MP_ASSERT(glGetError() == 0);



	while(!g_nQuit)
	{
		MICROPROFILE_SCOPE(MAIN);

		SDL_Event Evt;
		while(SDL_PollEvent(&Evt))
		{
			HandleEvent(&Evt);
		}
		if(g_nNumWorkers != nNumWorkers)
		{
			nNumWorkers = g_nNumWorkers;
			printf("NumWorkers %d\n", 1 + nNumWorkers % 8);
			JqStop();
			JqStart(1 + nNumWorkers % 8);
		}


		glClearColor(0.3f,0.4f,0.6f,0.f);
		glViewport(0, 0, WIDTH, HEIGHT);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		MicroProfileMouseButton(g_MouseDown0, g_MouseDown1);
		MicroProfileMousePosition(g_MouseX, g_MouseY, g_MouseDelta);
		g_MouseDelta = 0;


		MicroProfileFlip();
		{
			MICROPROFILE_SCOPEGPUI("GPU", "MicroProfileDraw", 0x88dd44);
			float projection[16];
			float left = 0.f;
			float right = WIDTH;
			float bottom = HEIGHT;
			float top = 0.f;
			float near = -1.f;
			float far = 1.f;
			memset(&projection[0], 0, sizeof(projection));

			projection[0] = 2.0f / (right - left);
			projection[5] = 2.0f / (top - bottom);
			projection[10] = -2.0f / (far - near);
			projection[12] = - (right + left) / (right - left);
			projection[13] = - (top + bottom) / (top - bottom);
			projection[14] = - (far + near) / (far - near);
			projection[15] = 1.f; 
 
 			glEnable(GL_BLEND);
 			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

			MicroProfileBeginDraw(WIDTH, HEIGHT, &projection[0]);
			MicroProfileDraw(WIDTH, HEIGHT);
			MicroProfileEndDraw();
			glDisable(GL_BLEND);
		}

		{
			MICROPROFILE_SCOPEI("MAIN", "Flip", 0xffee00);
			SDL_GL_SwapWindow(pWindow);
		}
		
		{
			JqTest();
		}

	}
	JqStop();

	MicroProfileShutdown();

  	SDL_GL_DeleteContext(glcontext);  
 	SDL_DestroyWindow(pWindow);
 	SDL_Quit();


	return 0;
}
