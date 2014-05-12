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
#include <thread>
#include <atomic>
#include <future>


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
// void JQ_USLEEP(__int64 usec) 
// { 
// 	if(usec > 20000)
// 	{
// 		Sleep((DWORD)(usec/1000));
// 	}
// 	else if(usec >= 1000)
// 	{
// 		timeBeginPeriod(1);
// 		Sleep((DWORD)(usec/1000));
// 		timeEndPeriod(1);
// 	}
// 	else
// 	{
// 		__int64 time1 = 0, time2 = 0, freq = 0;
// 		QueryPerformanceCounter((LARGE_INTEGER *) &time1);
// 		QueryPerformanceFrequency((LARGE_INTEGER *)&freq);

// 		do {
// 			QueryPerformanceCounter((LARGE_INTEGER *) &time2);
// 		} while((time2-time1)*1000000ll/freq < usec);
// 	}
// }
#endif


#define WIDTH 1024
#define HEIGHT 600


uint32_t g_nNumWorkers = 1;
uint32_t g_nQuit = 0;
uint32_t g_MouseX = 0;
uint32_t g_MouseY = 0;
uint32_t g_MouseDown0 = 0;
uint32_t g_MouseDown1 = 0;
int g_MouseDelta = 0;

MICROPROFILE_DEFINE(MAIN, "MAIN", "Main", 0xff0000);


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


#define JQ_IMPL
#define JQ_MICROPROFILE
#define JQ_MICROPROFILE_VERBOSE
#include "../jq.h"

#include <atomic>
std::atomic<int> g_nJobCount;
std::atomic<int> g_nJobCount0;
std::atomic<int> g_nJobCount1;
std::atomic<int> g_nJobCount2;
std::atomic<int> g_nLowCount;

#define JOB_COUNT 2
#define JOB_COUNT_0 3
#define JOB_COUNT_1 5
#define JOB_COUNT_2 10
#define JOB_COUNT_LOW 200

void JobTree2(void* pArg, int nIndex)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree2", 0xff);
	JQ_USLEEP(5+ rand() % 100);
	g_nJobCount2++;
}



void JobTree1(void* pArg, int nIndex)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree1", 0xff0000);
	JqAdd(JobTree2, nullptr, 2, JOB_COUNT_2);
	JQ_USLEEP(50 + rand() % 100);
	g_nJobCount1++;
}

void JobTree0(void* pArg, int nIndex)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree0", 0x00ff00);
	JqAdd(JobTree1, nullptr, 2, JOB_COUNT_1);
	JQ_USLEEP(50 + rand() % 100);
	((int*)pArg)[nIndex] = 1;
	g_nJobCount0++;
}

void JobTree(void* pArg, int nIndex)
{
	MICROPROFILE_SCOPEI("JQDEMO", "JobTree", 0xff5555);
	JQ_USLEEP(100);
	int lala[3]={0,0,0};
	uint64_t nJobTree0 = JqAdd(JobTree0, &lala[0], 2, 3);

	MICROPROFILE_SCOPEI("JQDEMO", "JobTree Wait", 0xff5555);
	JqWait(nJobTree0);
	g_nJobCount++;

}

void JqTest()
{
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST_WAIT_ALL", 0xff00ff);
		JqWaitAll();
	}
	MICROPROFILE_SCOPEI("JQDEMO", "JQ_TEST", 0xff00ff);

	g_nLowCount = 0;
	g_nJobCount = 0;
	g_nJobCount0 = 0;
	g_nJobCount1 = 0;
	g_nJobCount2 = 0;


	uint64_t nJob = JqAdd( [](void* arg, int idx)
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JobLow", 0x0000ff);
		JQ_USLEEP(200);
		g_nLowCount++;
	}, nullptr, 7, JOB_COUNT_LOW);
	{
		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
		JQ_USLEEP(500);
	}


	uint64_t nJobMedium = JqAdd(JobTree, nullptr, 0, JOB_COUNT);


	uint64_t nBatch = 0;
#if 1
	//test running out of job queue space.
	nBatch = JqAdd( [](void* arg, int idx)
	{
		for(int i = 0; i < 1200; ++i)
		{
			JqAdd( [](void* arg, int idx)
			{
				MICROPROFILE_SCOPEI("JQDEMO", "JobBulk", 0x00ff00);
				JQ_USLEEP(2);
			}, nullptr, 7, 1);
		}
	}, nullptr, 0, 1);
#endif
	{
		MICROPROFILE_SCOPEI("JQDEMO", "Sleep add1", 0x33ff33);
		JQ_USLEEP(500);
	}


	{
		if(0)
		{
			MICROPROFILE_SCOPEI("JQDEMO", "JqWaitSpin", 0xff0000);
			JqWait(nJob, JQ_WAITFLAG_SPIN);
		}
		else
		{
			JqWait(nJob);
		}
	}
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JqWaitMedium", 0xff0000);
		JqWait(nJobMedium);
	}
	{
		MICROPROFILE_SCOPEI("JQDEMO", "JqWaitBatch", 0xff0000);
		JqWait(nBatch);
	}

	JQ_ASSERT(g_nJobCount == JOB_COUNT);
	JQ_ASSERT(g_nJobCount0 == JOB_COUNT_0 * JOB_COUNT);
	JQ_ASSERT(g_nJobCount1 == JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
	JQ_ASSERT(g_nJobCount2 == JOB_COUNT_2 * JOB_COUNT_1 * JOB_COUNT_0 * JOB_COUNT);
	JQ_ASSERT(g_nLowCount == JOB_COUNT_LOW);
	


}



void MicroProfileQueryInitGL();
void MicroProfileDrawInit();
void MicroProfileBeginDraw(uint32_t nWidth, uint32_t nHeight, float* prj);
void MicroProfileEndDraw();


//typedef 
void CallTest(int (*foo)(int))
{
	printf("lala %d\n", (*foo)(32));
}

void CallTest(int (*foo)(int,int))
{
	printf("lala %d\n", (*foo)(5,5));
}

void CallTest_cap(std::function<int(int)> la)
{
	printf("capture %d\n", la(32));
}


int lala(int a)
{
	return 42;
}
std::function<int(int)> Getfunc(int a)
{
	int test = 1 + a;
	auto f = [=] (int foo)
	{
		printf("Getfunc lambda test %d foo %d\n", test, foo);
		return test * foo;
	};
	test += 1;
	printf("Getfunc::test variable %d\n", test);
	return f;
}
__thread uint32_t g_nDump = 0;
void* operator new (size_t size)
{
	//JQ_BREAK();
	if(g_nDump)
		printf("***** alloc %zu\n", size);
	void *p=malloc(size); 

	return p;
}

int JqGetRangeStart(int nIndex, int nNumJobs, int nNumElements)
{
	int nFraction = nNumElements / nNumJobs;
	int nRemainder = nNumElements - nFraction * nNumJobs;
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

void JqGetRange(int& nStart, int& nEnd, int nIndex, int nNumJobs, int nNumElements)
{
	nStart = JqGetRangeStart(nIndex, nNumJobs, nNumElements);
	nEnd = JqGetRangeStart(nIndex+1, nNumJobs, nNumElements);
}

int main(int argc, char* argv[])
{
	for(int i = 1; i < 148; ++i)
	{
		const int nNumJobs = i;
		for(int j = 0; j < 1024; ++j)
		{
			const int nRange = j;
			printf("testing %d jobs range %d\n", nNumJobs, nRange);
			int nCount = 0;
			int nLast = 0;
			for(int k = 0; k < nNumJobs; ++k)
			{
				int nStart, nEnd;
				JqGetRange(nStart, nEnd, k, nNumJobs, nRange);
				//printf("range %d :: %d:%d\n", nNumJobs, nStart, nEnd);
				JQ_ASSERT(nStart == nLast || nStart == nEnd);
				JQ_ASSERT(nStart <= nEnd);
				nCount += nEnd - nStart;
				nLast = nEnd;
			}
			JQ_ASSERT(nCount == nRange);
		}
	}
	int hest = 100;
	CallTest(lala);
	CallTest([](int x){ return x+112; });
	g_nDump = 1;
	CallTest([](int x,int y){ return x+y; });

	CallTest_cap([=](int x){ return x+hest; });
	int a[7];
	CallTest_cap([=](int x){ return a[x]; });


	auto f1 = Getfunc(1);
	auto f2 = Getfunc(2);

	CallTest_cap(f1);
	CallTest_cap(f2);

	printf("size is %lu\n", sizeof(f1));
	JQ_BREAK();
	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");

	if(SDL_Init(SDL_INIT_VIDEO) < 0) {
		return 1;
	}


	//JqTest();
	static uint32_t nNumWorkers = g_nNumWorkers;
	JqStart(nNumWorkers);

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


	std::mutex test;

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

		int lala = 0;
		{
			MICROPROFILE_SCOPEI("MUTEX_TEST", "MUTEX", -1);
			for(int i = 0; i < 10000; ++i)
			{
				test.lock();
				lala += 1;
				test.unlock();	
			}
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
