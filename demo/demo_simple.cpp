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
#include "microprofile.h"
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

// A list of small simple demos showing how the library works.
//

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

int main(int argc, char* argv[])
{

	MicroProfileOnThreadCreate("Main");

	JqAttributes Attr;
	JqInitAttributes(&Attr);
	// leave the Attributes default initialized.
	// This makes all workers pop in normal order [0 - (MJQ_MAX_QUEUES-1)], and makes a worker thread for each hw threads.
	JqStart(&Attr);
	// When we wait for jobs on a non-worker thread, we have to tell Jq what order we want it to pop jobs.
	// This needs to be done once per non-worker thread
	// Here we just pass in the default order which JqInitAttributes set up for us.
	JqSetThreadQueueOrder(&Attr.QueueOrder[0]);

	{
		// Simplest use case: A single job
		// The zero passed in is the Queue index
		JqHandle Handle = JqAdd(
			"Hello World",
			[] {
				printf("Hello World\n");
			},
			0);

		JqWait(Handle);
	}

	{
		// A single job, which is run 10 times
		// The ten passed in is the no. of times we want this job to be run
		JqHandle Handle = JqAdd(
			"Hello 10 World",
			[](int JobIndex) {
				printf("Hello 10 Worlds %d\n", JobIndex);
			},
			0, 10);

		JqWait(Handle);
	}

	{
		// Run 5 jobs, and split a range of 111 between them
		JqHandle Handle = JqAdd(
			"Hello 111 Worlds, with 5 jobs",
			[](int Begin, int End) {
				printf("Hello 111 Worlds [%d-%d]\n", Begin, End);
			},
			0, 5, 111);

		JqWait(Handle);
	}

	{
		// This sample shows jobs can be composed, and how waiting defaults to waiting on all child jobs
		// Run two jobs, which runs 2 jobs each, which again runs 3.

		JqHandle Handle = JqAdd(
			"ParentJob",
			[](int Parent) {
				JqAdd(
					"Child0",
					[Parent](int Child0) {
						JqAdd(
							"Child1",
							[Parent, Child0](int Child1) {
								printf("Parent/Child/Child %d/%d/%d\n", Parent, Child0, Child1);
							},
							0, 3);
					},
					0, 2);
			},
			0, 2);
		JqWait(Handle);
	}

	{
		// this shows two ways of not waiting for child jobs:
		JqHandle Parent0 = JqAdd(
			"Parent0",
			[] {
				JqAdd(
					"Child0",
					[] {

					},
					0, 1, -1, JQ_JOBFLAG_DETACHED); // passing in JQ_JOBFLAG_DETACHED detaches it from the parent and makes it a new 'root' job
			},
			0);
		JqWait(Parent0);

		JqHandle Parent1 = JqAdd(
			"Parent1",
			[] {
				JqAdd(
					"Child",
					[] {
					},
					0);
			},
			0);

		// passing in JQ_WAITFLAG_IGNORE_CHILDREN as a wait flag makes it ignore -all- child jobs
		// and only wait for Parent1
		JqWait(Parent1, JQ_DEFAULT_WAIT_FLAG | JQ_WAITFLAG_IGNORE_CHILDREN);
	}

	{
		// Successor: How to make job A run after B
		JqHandle A = JqAdd(
			"A",
			[] {
				printf("Job A\n");
			},
			0);
		JqHandle B = JqAddSuccessor(
			"B", A,
			[] {
				printf("Job B\n");
			},
			0);
		JqWait(B);
	}

	{
		// Blocked Job Handles.
		// JqCreateBlocked can be used to declare jobs, when multiple jobs want to interact with a job.
		// It is reserved up front, and passed around
		// this case shows a job C, To be started by A, and waited for by B
		JqHandle C = JqCreateBlocked("C");

		JqHandle A = JqAdd(
			"A",
			[C] {
				JobSpinWork(50000); // spin for 50 ms, so we know the waiter is likely to hit before we add
				JqAddBlocked(
					C,
					[] {
						printf("C\n");
					},
					0);
			},
			0);

		JqHandle B = JqAdd(
			"B",
			[C] {
				printf("B Start\n");
				JqWait(C);
				printf("B Waited\n");
			},
			0);
		JqWait(B);
		JqWait(A);
	}

	{

		// Blocked handles can also be used to create full job graphs
		JqHandle Before0 = JqCreateBlocked("Before0");
		JqHandle Before1 = JqCreateBlocked("Before1");
		JqHandle Barrier = JqCreateBlocked("Barrier");
		JqHandle After0	 = JqCreateBlocked("After0");
		JqHandle After1	 = JqCreateBlocked("After1");

		// Add all the links
		JqAddPrecondition(Barrier, Before0); // Barrier will not start before Before0
		JqAddPrecondition(Barrier, Before1); // Barrier will not start before Before1

		JqAddPrecondition(After0, Barrier); // After0 will not starte before Barrier
		JqAddPrecondition(After1, Barrier); // After0 will not starte before Barrier

		JqAddBlocked(
			After0,
			[] {
				printf("After0\n");
			},
			0);
		JqAddBlocked(
			After1,
			[] {
				printf("After1\n");
			},
			0);

		// Barrier is released manually, which just means it has no job to execute - IE it is just a barrier
		JqRelease(Barrier);

		JqAddBlocked(
			Before0,
			[] {
				printf("Before0\n");
			},
			0);
		JqAddBlocked(
			Before1,
			[] {
				printf("Before1\n");
			},
			0);

		JqWait(After0);
		JqWait(After1);
	}

	{
		JqHandle Before0 = JqCreateBlocked("Before0");
		JqHandle Before1 = JqCreateBlocked("Before1");
		// Barriers can be simplified by using initializer lists
		JqHandle Barrier   = JqBarrier("Barrier", { Before0, Before1 });
		JqHandle Successor = JqAddSuccessor(
			"PostBarrier", Barrier,
			[] {
				printf("postbarrier-initializer-list\n");
			},
			0);
		JqRelease(Barrier);
		JqAddBlocked(
			Before0,
			[] {
				printf("Before0-initializer-list\n");
			},
			0);
		JqAddBlocked(
			Before1,
			[] {
				printf("Before1-initializer-list\n");
			},
			0);

		JqWait(Successor);
	}

	{
		// Manual Block & Release:
		// JqBlock And JqRelease can be used to manually modify the block count of a job.
		// JqCreateBlocked returns a job with a block count of 1
		// JqAddBlocked Decreases it by one and sets up the job
		// JqRelease decrements it by one
		//
		// This is intended for synchronization with external systems like a gpu

		JqHandle ManualJob = JqCreateBlocked("Manual");

		const int JobCount = 4;

		// Note: The Block starts out as 1, so we only increment JobCount-1 times
		for(int i = 0; i < JobCount - 1; ++i)
			JqBlock(ManualJob);

		JqHandle Successor = JqAddSuccessor(
			"Successor", ManualJob,
			[] {
				printf("Successor to manual job\n");
			},
			0);

		JqHandle DecrementJob = JqAdd(
			"DecrementJob",
			[ManualJob] {
				printf("Decrement Release\n");
				JqRelease(ManualJob);
			},
			0, JobCount);

		JqWait(Successor);
	}

	{
		// clang-format off
		JqHandle Barrier = JqCreateBlocked("Barrier");
		JqHandle Job0	 = JqCreateBlocked("Job0");
		JqHandle Job1	 = JqCreateBlocked("Job1");
		JqAddPrecondition(Barrier, Job0);
		JqAddPrecondition(Barrier, Job1);

		JqRelease(Barrier); // Release initial block count

		JqAddBlocked(Job0,[] { printf("Job0\n"); }, 0);
		JqAddBlocked(Job1,[] { printf("Job1\n"); }, 0);
		JqHandle Successor = JqAddSuccessor("Successor", Barrier,[] { printf("sucessor\n"); }, 0);
		JqWait(Successor);
		// clang-format on
	}

	{
		// Cancelling a job
		// Note that there is no way of checking whether a job actually ran.

		std::atomic<int> Count;
		Count					 = 0;
		std::atomic<int>* pCount = &Count;

		JqHandle Cancel = JqAdd(
			"Cancel",
			[pCount] {
				pCount->fetch_add(1);
			},
			0, 10000);
		JqCancel(Cancel);
		JqWait(Cancel);
		printf("Cancel ran %d times\n", Count.load());
	}
#define COUNT0 4000
#define COUNT1 2000
#define COUNT_GROUP 30

	{
		std::atomic<int> c0;
		std::atomic<int> c1;
		c0						 = 0;
		c1						 = 0;
		std::atomic<int>* pc0	 = &c0;
		std::atomic<int>* pc1	 = &c1;
		JqHandle		  group1 = JqGroupBegin("group1");
		JqAdd(
			"add0",
			[pc0]() {
				pc0->fetch_add(1);
			},
			0, COUNT0);
		JqAdd(
			"add1",
			[pc1]() {
				pc1->fetch_add(1);
			},
			0, COUNT1);
		JqGroupEnd();

		JqWait(group1);
		printf("c0=%d, c1=%d\n", c0.load(), c1.load());
		JQ_ASSERT(c0 == COUNT0);
		JQ_ASSERT(c1 == COUNT1);
	}
	{
		// test groups, and groups added from other jobs work.
		std::atomic<int> c0;
		std::atomic<int> c1;
		c0					  = 0;
		c1					  = 0;
		std::atomic<int>* pc0 = &c0;
		std::atomic<int>* pc1 = &c1;

		// groups in groups.
		JqHandle group1 = JqGroupBegin("group_in_group_l1");
		JqAdd(
			"job0",
			[pc0, pc1]() {
				JqHandle group_inner = JqGroupBegin("group_in_groupl_l2");
				JqAdd(
					"add0",
					[pc0]() {
						pc0->fetch_add(1);
					},
					0, COUNT0);
				JqAdd(
					"add1",
					[pc1]() {
						pc1->fetch_add(1);
					},
					0, COUNT1);
				JqGroupEnd();
			},
			0, COUNT_GROUP);
		JqGroupEnd();

		JqWait(group1);

		printf("c0=%d, c1=%d\n", c0.load(), c1.load());
		JQ_ASSERT(c0.load() == COUNT0 * COUNT_GROUP);
		JQ_ASSERT(c1.load() == COUNT1 * COUNT_GROUP);
	}

	// Stop Jq.
	JqStop();
	return 0;
}
