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


int main(int argc, char* argv[])
{

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");

	uint32_t nJqInitFlags = JQ_INIT_USE_SEPERATE_STACK;
	for(int i = 1; i < argc; ++i)
	{
		if(0 == strcmp("-ns", argv[i]))
		{
			printf("disabling seperate stack\n");
			nJqInitFlags &= ~JQ_INIT_USE_SEPERATE_STACK;
		}
	}
	#define ITER 1000

	for(int i = 1; i <= 64; i *= 2)
	{
		static JqAttributes Attr;
		JqInitAttributes(&Attr, i);
		Attr.Flags = nJqInitFlags;
		JqStart(&Attr);

		int64_t Begin = JqTick();
		int64_t NumJobs = 0;
		for(int j = 0; j < 10; ++j)
		{

			uint64_t Handle = JqAdd([]()
			{

			}, 0, i);
			NumJobs += i;
			JqWait(Handle);
		}
		int64_t End = JqTick();
		int64_t TPS = JqTicksPerSecond();
		double JobTime = (End - Begin) * 1000000000.0 / (TPS * NumJobs);
		printf("Time per job %02d %8d :: %12.8fns\n", i, NumJobs, JobTime);
		JqStop();
	}
	printf("***************************************************\n");


	for(int i = 1; i <= 64; i *= 2)
	{
		static JqAttributes Attr;
		JqInitAttributes(&Attr, i);
		Attr.Flags = nJqInitFlags;
		JqStart(&Attr);

		int64_t Begin = JqTick();
		int64_t NumJobs = 0;
		for(int j = 0; j < 10; ++j)
		{

			uint64_t Handle = JqAdd([]()
			{

			}, 0, ITER * i);
			NumJobs += ITER * i;
			JqWait(Handle);
		}
		int64_t End = JqTick();
		int64_t TPS = JqTicksPerSecond();
		double JobTime = (End - Begin) * 1000000000.0 / (TPS * NumJobs);
		printf("Time per job %02d %8d :: %12.8fns\n", i, NumJobs, JobTime);
		JqStop();
	}

	printf("***************************************************\n");


	for(int i = 1; i <= 64; i *= 2)
	{
		static JqAttributes Attr;
		JqInitAttributes(&Attr, i);
		Attr.Flags = nJqInitFlags;
		JqStart(&Attr);

		int64_t Begin = JqTick();
		int64_t NumJobs = 0;
		uint64_t Handles[ITER];
		for(int j = 0; j < ITER; ++j)
		{
			Handles[j] = JqAdd([]()
			{

			}, 0, i);
			NumJobs += i;
		}
		for(int j = 0; j < ITER; ++j)
		{
			JqWait(Handles[j]);
		}
		int64_t End = JqTick();
		int64_t TPS = JqTicksPerSecond();
		double JobTime = (End - Begin) * 1000000000.0 / (TPS * NumJobs);
		printf("Time per job %02d %8d :: %12.8fns\n", i, NumJobs, JobTime);
		JqStop();
	}


	printf("done\n");
	return 0;
}
