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
// int64_t JqTick();
// int64_t JqTicksPerSecond();

uint32_t g_Reset = 0;

#include <thread>

#include "../jq.h"
#include <atomic>
#include <string.h>

uint32_t JobSpinWork(uint32_t nUs)
{
	if(g_DontSleep)
	{
		return 0;
	}
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

void JqTestPrio()
{
	MICROPROFILE_SCOPEI("JQ_TEST", "JqTestPrio", MP_AUTO);
	g_DontSleep = 0;

	// Note here, that there is nothing to indicate this is a barrier, it might as well be added as a job
	// If its a job, we add it with JqAddReserved
	// If its a Barrier we just close it with JqCloseReserved
	// Note, that all Preconditions must be in place first.
	// The argument for Reserve is the queue it ends up in
	JqHandle Barrier = JqReserve(0);
	JqHandle Final	 = JqReserve(0);

	std::atomic<int> TwoStart;
	std::atomic<int> TwoEnd;
	TwoStart					= 0;
	TwoEnd						= 0;
	std::atomic<int>* pTwoStart = &TwoStart;
	std::atomic<int>* pTwoEnd	= &TwoEnd;

	// printf("Two two %d %d\n", pTwoStart->load(), pTwoEnd->load());
	{
		// different ways of adding dependent jobs
		JqHandle PostBarrier0 = JqReserve(0);
		// PostBarrier0 now requires Barrier to be reached
		JqAddPrecondition(PostBarrier0, Barrier);

		JqAddReserved(
			PostBarrier0,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "First PostBarrier", MP_AUTO);
				JobSpinWork(500);
			},
			5);

		// alternative way of adding, when there is only one precondtion
		JqHandle PostBarrier1 = JqAddSuccessor(
			Barrier,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "Second PostBarrier", MP_AUTO);
				JobSpinWork(500);
			},
			0, 5);
		JqHandle PostBarrier2 = JqAddSuccessor(
			Barrier,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "Third PostBarrier", MP_AUTO);
				JobSpinWork(500);
			},
			0, 10);

		JqAddPrecondition(Final, PostBarrier0);
		JqAddPrecondition(Final, PostBarrier1);
		JqAddPrecondition(Final, PostBarrier2);
	}

	JqHandle J1 = JqAdd(
		[](int index) {
			MICROPROFILE_SCOPEI("JQ_TEST", "BASE-7-JOBS", MP_GREY);
			JobSpinWork(5000);
		},
		7, 3);
	JqHandle J2 = JqAdd(
		[pTwoStart, pTwoEnd] {
			pTwoStart->fetch_add(1);
			MICROPROFILE_SCOPEI("JQ_TEST", "BASE-1", MP_GREY);
			JqAdd(
				[] {
					MICROPROFILE_SCOPEI("JQ_TEST", "BASE-1-CHILD", MP_DARKSLATEGREY);
					JqAdd([] { MICROPROFILE_SCOPEI("JQ_TEST", "P5", 0xffff00); }, 5, 500);
					JobSpinWork(20);
				},
				1, 20);
			JobSpinWork(2000);
			pTwoEnd->fetch_add(1);
		},
		0, 2, 1);

	JqAddPrecondition(Barrier, J1);
	JqAddPrecondition(Barrier, J2);

	std::atomic<int>  Foo;
	std::atomic<int>  Bar;
	std::atomic<int>  SuccessorDone;
	std::atomic<int>  ReservedSuccessorDone;
	std::atomic<int>* pFoo					 = &Foo;
	std::atomic<int>* pBar					 = &Bar;
	std::atomic<int>* pSuccessorDone		 = &SuccessorDone;
	std::atomic<int>* pReservedSuccessorDone = &ReservedSuccessorDone;

	JqHandle ReservedHandle = JqReserve(0);
	Foo						= 0;
	Bar						= 0;
	SuccessorDone			= 0;
	struct
	{
		JqHandle ReservedHandle;
		uint32_t Frame;
	} H;
	static uint32_t Frame = 0;
	Frame++;
	H.ReservedHandle = ReservedHandle;
	H.Frame			 = Frame;

	JqHandle ReservedWait = JqAdd(
		[pBar, ReservedHandle] {
			MICROPROFILE_SCOPEI("JQ_TEST", "Reserved_WAIT", MP_PINK);
			JqWait(ReservedHandle);
			if(2 != *pBar)
			{
				uprintf("Reserved wait failed!");
				JQ_BREAK();
			}
		},
		1, 1);

	JqHandle J3 = JqAdd(
		[pFoo, pBar, &H](int JobIndex) {
			MICROPROFILE_SCOPEI("JQ_TEST", "Lots of increments", (H.Frame & 1) == 1 ? MP_YELLOW : MP_CYAN);
			JobSpinWork(50);
			pFoo->fetch_add(1);
			// printf("JOB INDEX IS %d\n", JobIndex);
			if(JobIndex == 999)
			{
				// printf("HERE HERE JOB INDEX IS %d\n", JobIndex);
				JqAddReserved(
					H.ReservedHandle,
					[pBar] {
						MICROPROFILE_SCOPEI("JQ_TEST", "RESERVED", MP_PINK);
						JobSpinWork(5000);

						pBar->fetch_add(1);
					},
					2);
			}
		},
		0, 1000);

	JqHandle Successor = JqAddSuccessor(
		J3,
		[pFoo, pSuccessorDone]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "THE_SUCCESSOR", MP_RED);
			if(1000 != *pFoo)
			{
				JQ_BREAK();
			}
			JobSpinWork(5000);

			*pSuccessorDone = 1;
		},
		0);

	JqHandle ReservedSuccessor = JqAddSuccessor(
		ReservedHandle,
		[pReservedSuccessorDone, pBar]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "THE_RESERVEDSUCCESSOR", MP_RED);
			if(2 != *pBar)
			{
				JQ_BREAK();
			}

			*pReservedSuccessorDone = 1;
			JobSpinWork(5000);
		},
		0);

	JqAddPrecondition(Final, Successor);

	JqCloseReserved(Barrier);

	JqAddReserved(
		Final,
		[]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "Final", MP_AUTO);
			JobSpinWork(500);
		},
		1);

	for(uint32_t i = 0; i < 10; ++i)
	{
		JqAddSuccessor(
			Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-1", MP_GREY);
				// JobSpinWork(100);
			},
			0, 32);
		JqAddSuccessor(
			Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-2", MP_GREY);
				// JobSpinWork(100);
			},
			0, 32);
		JqAddSuccessor(
			Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-3", MP_GREY);
				// JobSpinWork(100);
			},
			0, 32);
		JqAddSuccessor(
			Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-4", MP_GREY);
				// JobSpinWork(100);
			},
			0, 32);
	}
	JqWait(J1, JQ_WAITFLAG_EXECUTE_ANY | JQ_WAITFLAG_SLEEP);
	JqWait(J2);
	JqWait(J3);
	JqWait(ReservedWait);
	JqWait(ReservedHandle);
	JqWait(Successor);
	JqWait(ReservedSuccessor);

	if(0 == *pReservedSuccessorDone)
	{
		JQ_BREAK();
	}
	if(0 == *pSuccessorDone)
	{
		JQ_BREAK();
	}
}

struct SCancelJobState
{
	JqHandle		 Handle;
	std::atomic<int> Started;
	std::atomic<int> Finished;
	int				 Cancelled;
	int				 CancelRequested;
};

void JqTestCancel()
{
	std::atomic<int> CancelFinished;
	const int		 FAN_OUT	 = 10;
	int				 nNumWorkers = JqGetNumWorkers();
	int				 nNumJobs	 = nNumWorkers * 2;
	SCancelJobState* Cancel		 = new SCancelJobState[nNumJobs];
	CancelFinished				 = 0;
	// start a bunch of jobs, cancel half of them
	JqHandle nGroup = JqGroupBegin();
	for(int i = 0; i < nNumJobs; ++i)
	{
		SCancelJobState* pState	  = &Cancel[i];
		Cancel[i].Started		  = 0;
		Cancel[i].Finished		  = 0;
		Cancel[i].Cancelled		  = 0;
		Cancel[i].CancelRequested = 0;
		Cancel[i].Handle		  = JqAdd(
			 [pState, &CancelFinished] {
				 MICROPROFILE_SCOPEI("CANCEL", "WORK", MP_CYAN);
				 pState->Started.fetch_add(1);
				 JobSpinWork(10);
				 pState->Finished.fetch_add(1);
				 CancelFinished.fetch_add(1);
			 },
			 0, 10);
	}
	JqGroupEnd();
	int nNumCancelled = 0;
	{
		MICROPROFILE_SCOPEI("CANCEL", "CANCEL", MP_CYAN);
		JobSpinWork(2000);
		if(1)
		{
			for(int i = 0; i < nNumJobs; ++i)
			{
				do
				{
					int				 idx	= rand() % nNumJobs;
					SCancelJobState* pState = &Cancel[idx];
					if(!pState->CancelRequested)
					{
						uint32_t CancelGuaranteed = 0;
						JqCancel(pState->Handle);
						pState->Cancelled		= CancelGuaranteed;
						pState->CancelRequested = 1;
						break;
					}
				} while(1);
			}
		}
	}
	JqWait(nGroup);
	delete[] Cancel;
}

void JqTestSpawn()
{
	std::atomic<int> v;
	v						   = 0;
	std::atomic<int>* pv	   = &v;
	uint64_t		  ThreadId = JqGetCurrentThreadId();

	// verify its never run
	JqSpawn([] { JQ_BREAK(); }, 0, 0);

	// verify its always run on current thread
	JqSpawn(
		[ThreadId, pv] {
			pv->fetch_add(1);
			if(ThreadId != JqGetCurrentThreadId())
				JQ_BREAK();
		},
		0, 1);
	if(v.load() != 1)
	{
		JQ_BREAK();
	}

	// verify its run 50 times
	JqSpawn(
		[ThreadId, pv](int index) {
			pv->fetch_add(1);
			if(index == 0)
				if(ThreadId != JqGetCurrentThreadId())
					JQ_BREAK();
		},
		0, 50);
	if(v.load() != 51)
	{
		JQ_BREAK();
	}
}

void JqTestWaitExecuteChildren()
{
	printf("todo: test this");
}

void JqTestWaitIgnoreChildren()
{
	std::atomic<int> parent, child;
	parent = 0;
	child  = 0;

	std::atomic<int>* pparent = &parent;
	std::atomic<int>* pchild  = &child;
#define PARENTS 5
#define CHILDREN 50

	JqHandle Job = JqAdd(
		[pparent, pchild] {
			pparent->fetch_add(1);

			JqAdd(
				[pchild] {
					JobSpinWork(1000);
					pchild->fetch_add(1);
				},
				0, CHILDREN);
		},
		0, PARENTS);

	JqWait(Job, JQ_WAITFLAG_IGNORE_CHILDREN | JQ_DEFAULT_WAIT_FLAG);
	if(parent.load() != PARENTS)
		JQ_BREAK();
	if(child.load() > CHILDREN * PARENTS)
		JQ_BREAK();

	if(child.load() == CHILDREN * PARENTS)
		printf("all children executed. this hints that JQ_WAITFLAG_IGNORE_CHILDREN might not ignore children");

	JqWait(Job);
	if(child.load() != CHILDREN * PARENTS)
		JQ_BREAK();

#undef PARENTS
#undef CHILDREN
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

	printf("press 'z' to toggle microprofile drawing\n");
	printf("press 'right shift' to pause microprofile update\n");
	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif
	static JqAttributes Attr;
	JqInitAttributes(&Attr, 5, 0);
	Attr.Flags		   = nJqInitFlags;
	Attr.QueueOrder[0] = JqQueueOrder{ 7, { 0, 1, 2, 3, 4, 5, 6, 0xff } };
	Attr.QueueOrder[1] = JqQueueOrder{ 3, { 3, 2, 1, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.QueueOrder[2] = JqQueueOrder{ 2, { 5, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.QueueOrder[3] = JqQueueOrder{ 2, { 1, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.QueueOrder[4] = JqQueueOrder{ 1,
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

	for(uint32_t i = 0; i < Attr.NumWorkers; ++i)
	{
		Attr.WorkerOrderIndex[i] = i < 5 ? i : 0;
	}

	JqStart(&Attr);
	JqQueueOrder MyQueueConfig = JqQueueOrder{ 2, { 0, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	JqSetThreadQueueOrder(&MyQueueConfig);

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
			JqTestPrio();
			JqTestCancel();
			JqTestSpawn();
			JqTestWaitIgnoreChildren();
		}
		JqLogStats();
	}
#ifdef _WIN32
	foo.join();
#endif
	JqStop();
	return 0;
}
