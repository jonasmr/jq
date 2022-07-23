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
int		 g_LimitedMode = 0;

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
extern uint32_t g_TESTID;

void JqTestPrio(uint32_t NumWorkers)
{
	MICROPROFILE_SCOPEI("JQ_TEST", "JqTestPrio", MP_AUTO);
	g_DontSleep = 0;

	// Note here, that there is nothing to indicate this is a barrier, it might as well be added as a job
	// If its a job, we add it with JqAddReserved
	// If its a Barrier we just close it with JqCloseReserved
	// Note, that all Preconditions must be in place first.
	// The argument for Reserve is the queue it ends up in
	JqHandle Barrier = JqReserve("Barrier");
	JqHandle Final	 = JqReserve("Final");

	std::atomic<int> TwoStart;
	std::atomic<int> TwoEnd;
	TwoStart					= 0;
	TwoEnd						= 0;
	std::atomic<int>* pTwoStart = &TwoStart;
	std::atomic<int>* pTwoEnd	= &TwoEnd;

	// printf("Two two %d %d\n", pTwoStart->load(), pTwoEnd->load());
	{
		// different ways of adding dependent jobs
		JqHandle PostBarrier0 = JqReserve("PostBarrier0");
		// PostBarrier0 now requires Barrier to be reached
		JqAddPrecondition(PostBarrier0, Barrier);

		JqAddReserved(
			PostBarrier0,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "First PostBarrier", MP_AUTO);
				JobSpinWork(100);
			},
			0, 5);

		// alternative way of adding, when there is only one precondtion
		JqHandle PostBarrier1 = JqAddSuccessor(
			"PostBarrier1", Barrier,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "Second PostBarrier", MP_AUTO);
				JobSpinWork(100);
			},
			0, 1);
		JqHandle PostBarrier2 = JqAddSuccessor(
			"PostBarrier2", Barrier,
			[]() {
				MICROPROFILE_SCOPEI("JQ_TEST", "Third PostBarrier", MP_AUTO);
				JobSpinWork(100);
			},
			0, 1);

		JqAddPrecondition(Final, PostBarrier0);
		JqAddPrecondition(Final, PostBarrier1);
		JqAddPrecondition(Final, PostBarrier2);
	}

	JqHandle J1 = JqAdd(
		"J1",
		[](int index) {
			MICROPROFILE_SCOPEI("JQ_TEST", "BASE-7-JOBS", MP_GREY);
			JobSpinWork(50);
		},
		7, 3);
	JqHandle J2 = JqAdd(
		"J2",
		[pTwoStart, pTwoEnd] {
			pTwoStart->fetch_add(1);
			MICROPROFILE_SCOPEI("JQ_TEST", "BASE-1", MP_GREY);
			JqAdd(
				"Base-1",
				[] {
					MICROPROFILE_SCOPEI("JQ_TEST", "BASE-1-CHILD", MP_DARKSLATEGREY);
					JqAdd(
						"P5",
						[] {
							MICROPROFILE_SCOPEI("JQ_TEST", "P5", 0xffff00);
						},
						5, 500);
					JobSpinWork(20);
				},
				1, g_LimitedMode ? 2 : 20);
			JobSpinWork(20);
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

	JqHandle ReservedHandle = JqReserve("ReservedHandle");
	Foo						= 0;
	Bar						= 0;
	SuccessorDone			= 0;
	struct
	{
		JqHandle ReservedHandle;
		uint32_t Frame;
		uint32_t NumJobs;
	} H;
	static uint32_t Frame = 0;
	Frame++;
	H.ReservedHandle = ReservedHandle;
	H.Frame			 = Frame;
	H.NumJobs		 = 32 * NumWorkers;

	JqHandle ReservedWait = JqAdd(
		"ReservedWait",
		[pBar, ReservedHandle] {
			MICROPROFILE_SCOPEI("JQ_TEST", "Reserved_WAIT", MP_PINK);
			JqWait(ReservedHandle);
			if(2 != *pBar)
			{
				printf("Reserved wait failed!");
				JQ_BREAK();
			}
		},
		1, 1);

	JqHandle J3 = JqAdd(
		"J3",
		[pFoo, pBar, &H](int JobIndex) {
			MICROPROFILE_SCOPEI("JQ_TEST", "Lots of increments", (H.Frame & 1) == 1 ? MP_YELLOW : MP_CYAN);
			JobSpinWork(50);
			pFoo->fetch_add(1);
			// printf("JOB INDEX IS %d\n", JobIndex);
			if(JobIndex == H.NumJobs - 1)
			{
				// printf("HERE HERE JOB INDEX IS %d\n", JobIndex);
				JqAddReserved(
					H.ReservedHandle,
					[pBar] {
						MICROPROFILE_SCOPEI("JQ_TEST", "RESERVED", MP_PINK);
						JobSpinWork(100);

						pBar->fetch_add(1);
					},
					0, 2);
			}
		},
		0, H.NumJobs);

	JqHandle Successor = JqAddSuccessor(
		"Successor", J3,
		[pFoo, pSuccessorDone, &H]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "THE_SUCCESSOR", MP_RED);
			if(H.NumJobs != *pFoo)
			{
				JQ_BREAK();
			}
			JobSpinWork(100);

			*pSuccessorDone = 1;
		},
		0);

	JqHandle ReservedSuccessor = JqAddSuccessor(
		"ReservedSucessor", ReservedHandle,
		[pReservedSuccessorDone, pBar]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "THE_RESERVEDSUCCESSOR", MP_RED);
			if(2 != *pBar)
			{
				JQ_BREAK();
			}

			*pReservedSuccessorDone = 1;
			JobSpinWork(100);
		},
		0);

	JqAddPrecondition(Final, Successor);

	JqRelease(Barrier);

	JqAddReserved(
		Final,
		[]() {
			MICROPROFILE_SCOPEI("JQ_TEST", "Final", MP_AUTO);
			JobSpinWork(100);
		},
		0);

	for(uint32_t i = 0; i < 3; ++i)
	{
		uint32_t NumJobs = NumWorkers / 2;
		JqAddSuccessor(
			"JOB_32-1", Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-1", MP_GREY);
				// JobSpinWork(100);
			},
			0, NumJobs);
		JqAddSuccessor(
			"JOB_32-2", Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-2", MP_GREY);
				// JobSpinWork(100);
			},
			0, NumJobs);
		JqAddSuccessor(
			"JOB_32-3", Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-3", MP_GREY);
				// JobSpinWork(100);
			},
			0, NumJobs);
		JqAddSuccessor(
			"JOB_32-4", Final,
			[] {
				MICROPROFILE_SCOPEI("JQ_TEST", "JOB_32-4", MP_GREY);
				// JobSpinWork(100);
			},
			0, NumJobs);
	}
	{
		// test JQ_WAITFLAG_EXECUTE_CHILDREN.
		// by now there is a ton of work, so it'll be a while. make a small job tree, and wait on it here.
		uint64_t		 ThreadId = JqGetCurrentThreadId();
		std::atomic<int> v;
		v = 0;
		std::atomic<int> x;
		x					 = 0;
		std::atomic<int>* pv = &v;
		std::atomic<int>* px = &x;
#define FANOUT 3
		MICROPROFILE_SCOPEI("ChildWait", "Child-Small-Tree-Time", MP_AUTO);
		JobSpinWork(100);

		if(1)
		{
			JqHandle SmallTree = JqAdd(
				"Child-Small-Tree-0",
				[pv, ThreadId, px] {
					px->fetch_add(1);
					MICROPROFILE_SCOPEI("ChildWait", "Child-Small-Tree-0", MP_AUTO);
					JqAdd(
						"Child-Small-Tree-1",
						[pv, ThreadId] {
							//							printf("tree 0 %p\n", JqGetCurrentThreadId());
							MICROPROFILE_SCOPEI("ChildWait", "Child-Small-Tree-1", MP_AUTO);
							JqAdd(
								"Child-Small-Tree-2",
								[pv, ThreadId] {
									//									printf("tree 1 %p\n", JqGetCurrentThreadId());
									MICROPROFILE_SCOPEI("ChildWait", "Child-Small-Tree-2", MP_AUTO);
									if(ThreadId == JqGetCurrentThreadId())
									{
										MICROPROFILE_SCOPEI("ChildWait", "Child-Wait-ok", MP_AUTO);
										pv->fetch_add(1);
									}
									else
									{
										MICROPROFILE_SCOPEI("ChildWait", "Child-Wait-fail", MP_AUTO);
									}
								},
								0, FANOUT);
						},
						0, FANOUT);

					px->fetch_add(0x100);
				},
				0, FANOUT);
			MICROPROFILE_SCOPEI("ChildWait", "ChildWaitTime", MP_AUTO);
			JqWait(SmallTree, JQ_WAITFLAG_EXECUTE_CHILDREN | JQ_WAITFLAG_SPIN);
			// if(v.load() != (FANOUT * FANOUT * FANOUT))
			// {
			// 	printf("\nonly %d was executed locally\n", v.load());
			// }
		}
#undef FANOUT
	}

	{
		// Test JqSpawn
		std::atomic<int> v;
		v						   = 0;
		std::atomic<int>* pv	   = &v;
		uint64_t		  ThreadId = JqGetCurrentThreadId();

		// verify its never run
		JqSpawn(
			"Spawn0",
			[] {
				JQ_BREAK();
			},
			0, 0);

		// verify its always run on current thread
		JqSpawn(
			"Spawn1",
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

#define TIMES 50
		std::atomic<int> data[TIMES];
		for(std::atomic<int>& d : data)
			d.store(0);
		JqSpawn(
			"Spawn50",
			[ThreadId, pv, &data](int index) {
				data[index].fetch_add(1);
				pv->fetch_add(1);
				if(index == 0)
					if(ThreadId != JqGetCurrentThreadId())
						JQ_BREAK();
			},
			0, TIMES);
		for(int i = 0; i < TIMES; ++i)
		{
			if(data[i].load() != 1)
			{
				JQ_BREAK();
			}
		}
		if(v.load() != 51)
		{
			JQ_BREAK();
		}
	}
#undef TIMES

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
	if(g_LimitedMode)
		nNumJobs = 2;
	SCancelJobState* Cancel = new SCancelJobState[nNumJobs];
	CancelFinished			= 0;
	// start a bunch of jobs, cancel half of them
	JqHandle nGroup = JqGroupBegin("TestCancelGroup");
	for(int i = 0; i < nNumJobs; ++i)
	{
		SCancelJobState* pState	  = &Cancel[i];
		Cancel[i].Started		  = 0;
		Cancel[i].Finished		  = 0;
		Cancel[i].Cancelled		  = 0;
		Cancel[i].CancelRequested = 0;
		Cancel[i].Handle		  = JqAdd(
			 "Cancel",
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

void JqTestWaitIgnoreChildren()
{
	MICROPROFILE_SCOPEI("JQ_TEST", "JqTestWaitIgnoreChildren", MP_AUTO);
	std::atomic<int> parent, child;
	parent = 0;
	child  = 0;

	std::atomic<int>* pparent = &parent;
	std::atomic<int>* pchild  = &child;
#define PARENTS 1
#define CHILDREN 250

	JqHandle Job = JqAdd(
		"JqTestWaitIgnoreChildren_PARENT_JOB",
		[pparent, pchild] {
			MICROPROFILE_SCOPEI("JQ_TEST", "JqTestWaitIgnoreChildren_PARENT_JOB", MP_PINK);
			pparent->fetch_add(1);
			JobSpinWork(100);
			JqAdd(
				"JqTestWaitIgnoreChildren_CHILD_JOB",
				[pchild] {
					MICROPROFILE_SCOPEI("JQ_TEST", "JqTestWaitIgnoreChildren_CHILD_JOB", MP_BLUE);
					JobSpinWork(10);
					pchild->fetch_add(1);
				},
				0, CHILDREN);
		},
		0, PARENTS);

	{
		MICROPROFILE_SCOPEI("JQ_TEST", "WaitIgnore", MP_PINK);
		JqWait(Job, JQ_WAITFLAG_IGNORE_CHILDREN | JQ_DEFAULT_WAIT_FLAG);
	}
	if(parent.load() != PARENTS)
		JQ_BREAK();
	if(child.load() > CHILDREN * PARENTS)
		JQ_BREAK();
#if 0
	if(child.load() == CHILDREN * PARENTS)
		printf("all children executed. this hints that JQ_WAITFLAG_IGNORE_CHILDREN might not ignore children");
#endif

	{
		MICROPROFILE_SCOPEI("JQ_TEST", "WaitFull", MP_BLUE);
		JqWait(Job);
	}

	if(child.load() != CHILDREN * PARENTS)
		JQ_BREAK();

#undef PARENTS
#undef CHILDREN
}

int main(int argc, char* argv[])
{

	uint32_t nJqInitFlags  = JQ_INIT_USE_SEPERATE_STACK;
	bool	 UseMinWorkers = false;
	for(int i = 1; i < argc; ++i)
	{
		if(0 == strcmp("-ns", argv[i]))
		{
			printf("disabling seperate stack\n");
			nJqInitFlags &= ~JQ_INIT_USE_SEPERATE_STACK;
		}
		if(0 == strcmp("-minworkers", argv[i]))
		{
			UseMinWorkers = true;
		}
		if(0 == strcmp("-limited", argv[i]))
		{
			g_LimitedMode = 1;
		}
	}

	MicroProfileOnThreadCreate("Main");
#ifdef _WIN32
	ShowWindow(GetConsoleWindow(), SW_MAXIMIZE);
#endif
	static JqAttributes Attr;
	JqInitAttributes(&Attr, 5, 0);
	if(Attr.NumWorkers < 4)
	{
		printf("Demo won't run on hardware with < 4 threads\n");
		exit(1);
	}
	if(UseMinWorkers)
		Attr.NumWorkers = 4;

	Attr.Flags		   = nJqInitFlags;
	Attr.QueueOrder[0] = JqQueueOrder{ 7, { 0, 1, 2, 3, 4, 5, 6, 0xff } };
	Attr.QueueOrder[1] = JqQueueOrder{ 3, { 3, 2, 1, 0xff, 0xff, 0xff, 0xff, 0xff } };
	Attr.QueueOrder[2] = JqQueueOrder{ 2, { 5, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	if(Attr.NumWorkers == 4)
	{
		// with only 4 workers, we need to make sure 7 is drained
		Attr.QueueOrder[3] = JqQueueOrder{ 3, { 1, 5, 7, 0xff, 0xff, 0xff, 0xff, 0xff } };
	}
	else
	{
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
	}

	for(uint32_t i = 0; i < Attr.NumWorkers; ++i)
	{
		Attr.WorkerOrderIndex[i] = i < 5 ? i : 0;
	}

	JqStart(&Attr);
	JqQueueOrder MyQueueConfig = JqQueueOrder{ 2, { 0, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
	JqSetThreadQueueOrder(&MyQueueConfig);
	printf("Started JQ with %d workers\n", Attr.NumWorkers);
	printf("press 'q' to quit\n");

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
			case 'q':
				g_nQuit = 1;
				break;
			}
		}
		MicroProfileFlip(0);
		static int Frames = 0;
		if(++Frames == 10)
		{
			// to convert the graph run: dot -Tps graphdump.gv -o graphdump.ps
			JqGraphDumpStart("graphdump.gv", 1024 * 1024);
		}
		if(Frames == 13)
		{
			JqGraphDumpEnd();
		}

		{
			JqTestPrio(Attr.NumWorkers);
			JqTestCancel();
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
