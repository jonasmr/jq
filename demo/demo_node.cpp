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

uint32_t g_nNumWorkers = 1;
uint32_t g_nQuit = 0;

MICROPROFILE_DEFINE(MAIN, "MAIN", "Main", 0xff0000);
#ifdef _WIN32
#define DEMO_ASSERT(a) do{if(!(a)){__debugbreak();} }while(0)
#else
#define DEMO_ASSERT(a) do{if(!(a)){__builtin_trap();} }while(0)
#endif

int64_t JqTick();
int64_t JqTicksPerSecond();



#include <thread>

#include "../jq.h"
#include "../jqnode.h"


#define JQ_TEST_WORKERS 5

int main(int argc, char* argv[])
{
	uint32_t nJqInitFlags = JQ_INIT_USE_SEPERATE_STACK;
	for(uint32_t i = 1; i < argc; ++i)
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
	static uint32_t nNumWorkers = g_nNumWorkers;
	(void)nNumWorkers;
	uint8_t nPipeConfig[JQ_NUM_PIPES * JQ_TEST_WORKERS] = 
	{
		0, 1, 2, 3,				4, 5, 6, 0xff,
		3, 2, 1, 0xff,			0xff, 0xff, 0xff, 0xff,
		5, 1, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,
		1, 5, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,
		7, 0xff, 0xff, 0xff,		0xff, 0xff, 0xff, 0xff,

	};
	//JQ_NUM_PIPES
	JqStart(JQ_TEST_WORKERS, sizeof(nPipeConfig), nPipeConfig, nJqInitFlags);

	uint8_t MyPipeConfig[JQ_NUM_PIPES] =
	{
		0,5, 0xff, 0xff,	0xff, 0xff, 0xff, 0xff,
	};

	JqSetThreadPipeConfig(MyPipeConfig);

	JqNode A(
		[]
		{
			printf("NODE A %d-%d\n",0,0);
		}, 1, 3);
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

	JqStop();
	return 0;
}
