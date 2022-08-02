# Jq

![jq-build](https://github.com/jonasmr/jq-private2/actions/workflows/ng-build.yml/badge.svg)

Jq is a minimal job queue, that can be used to multithread work across multiple cpus. 

* github: https://github.com/jonasmr/jq.git


Jq is designed to be as simple as possible, to ensure multithreaded code is easy to both write and read, while also having a focus on efficiency. 

* Adding and waiting for a job is done by calling JqAdd/JqWait
* Jq only allocates memory on startup
* Jq has builtin support for jobs that execute multiple times, and has helper functionality to split larger ranges between those jobs
* Jobs in Jq are composable: If you create a job, and wait for that job, the default behaviour is to wait for the jobs it spawns
* Jq has an interface to add preconditions to jobs, which can be used to express job ordering and to do barrier like constructs
# Demo programs
Are located in `demo/`. 
* `demo_simple.cpp`: simple demo program
* `demo_profile.cpp`: profile job startup time with different no. of worker threads
* `demo_full.cpp`: fully stress test of most systems

## Building 
requires python & ninja to build.
run `ng`(osx/linux) `ng.bat` (windows - run from native tools command prompt)
`ninja all` to build all samples.


# Basic Usage
Jq is initialized by calling JqStart, passing in a `JqAttributes` object.
```
JqAttributes Attr;
JqInitAttributes(&Attr);
JqStart(&Attr);
```

This sets up Jq, and starts a worker thread for each hardware thread.

`JqStop` is called to shutdown Jq. Note that this will wait for all jobs to finish, so be aware that this won't return if jobs are running.
```
	JqStop();
``` 


Adding a job is done by calling `JqAdd`. `JqAdd` returns a `JqHandle` handle, which can be passed to JqWait to wait for jobs to finish. `JqHandle` is a thin wrapper containing a `uint64`.

Note that the lambda captured is defined to be at most `JQ_FUNCTION_SIZE` bytes(default:64), and any captured values must not have destructors, as these will not be called. Violation of these constraints are reported at compile time.

All functions adding jobs take a `const char*`, which can be used when debugging the state of the job system and when dumping the full job graph using `JqGraphDumpStart`/`JqGraphDumpEnd`

```
int Queue = 0;
JqHandle JobHandle = JqAdd("simple", []
{
	printf("test\n");
}, Queue);
JqWait(JobHandle);
```

`JqAdd` Optionally takes a number of times to invoke job, and a range to split between the jobs:

```
int Queue = 0;
JqAdd("range", [](int begin, int end)
{
	printf("range %d %d\n", begin, end);
}, PipeId, 2, 100);
```
This will invoke the printf job twice, passing in begin/end range such that the entire range (0-100) is covered
IE it might print
range 0 50
range 50 100


By Default, a job added from another job is a child of that job. A parent job is not considered finished untill all children are finished.

```
int Queue = 0;
JqHandle H = JqAdd("H",[=]
{
	print("parent"\n");
	JqAdd("Child", [],
	{
		printf("child\n");
	}, queue);
}, PipeId);
JqWait(H);
print("done\n");
```
prints 
```
parent
child
done
```


Its configurable what Jq should do when you tell it to wait for another job, using the `JobFlags` argument
* How to find jobs when the job we are waiting for is not finished
	* `JQ_WAITFLAG_EXECUTE_CHILDREN`: Only execute jobs that are children of the current job
	* `JQ_WAITFLAG_EXECUTE_PREFER_CHILDREN`: Prefer child jobs, but allow other jobs to run in case of no child jobs available
	* `JQ_WAITFLAG_EXECUTE_ANY`: Execute any job
* What to do when the jobs is not done, and there isnt an available candidate job to run instead
	* `JQ_WAITFLAG_BLOCK`: Wait on a semaphore which is signalled by job when done
	* `JQ_WAITFLAG_SLEEP`: Sleep for the amount specified when calling
	* `JQ_WAITFLAG_SPIN`: Spin untill done.
* `JQ_WAITFLAG_IGNORE_CHILDREN` can be used to explicitly wait for _only_ the job, and not its children


## Other useful functions
* `JqCancel` Can be called to cancel a Job. It will still proceed through the queue, but the job will not execture
* `JqSpawn` Starts a job and immediately waits for it. It is guaranteed that instance 0 will run on the calling thread
* `JqExecuteOne`: can be called from any thread to execute a job. 
* `JqExecuteChildren`: Execute one child job of job passed in.
* `JqConsumeStats`: Consume various internal stats.


## Job Groups

Jq supports adding groups of jobs. This is effectively a job that is never run, but is only used to group child jobs so they can be waited for together. This is useful when you have a large body of code adding a variable amount of work, to which you want to be able to wait for at a later point.

```
JqHandle H = JqGroupBegin("JobGroup");
JqAdd("a", [] {printf("a\n"), 0);
JqAdd("b", [] {printf("b\n"), 0);
JqGroupEnd();
JqWait(H); // will wait for print of a and b both
```


# Queues
`JqAttributes` is used to configure Jq. Always call `JqInitAttributers` to initialize it to its default state.

```
struct JqQueueOrder
{
	uint8_t NumQueues;
	uint8_t Queues[JQ_MAX_QUEUES];
};

struct JqAttributes
{
	uint32_t Flags;
	uint32_t NumWorkers;
	uint32_t StackSizeSmall;
	uint32_t StackSizeLarge;
	uint32_t NumQueueOrders;

	JqQueueOrder QueueOrder[JQ_MAX_THREADS];
	uint8_t		 WorkerOrderIndex[JQ_MAX_THREADS];
	uint64_t	 WorkerThreadAffinity[JQ_MAX_THREADS];
};
```

Jq always has `JQ_MAX_QUEUES` different queues where jobs can be pushed through. When using the default configuration, the queues behaves like priorities: All workers & waiters takes jobs in the queues as normal priority systems: Jobs from queues with lower index are executed first.

This is entirely configurable, using `JqQueueOrder`. This object specifies a number of queues to drain work from, and the order.

Below is an example that starts Jq with 4 worker threads, with the three first taking work from queue 0 first, then 1, and the last queue taking work from 1, then 2. 
```
JqInitAttributes(&Attr);
Attr.NumWorkers = 4;
Attr.NumQueueOrders = 2;
Attr.QueueOrder[0] = JqQueueOrder{ 2, { 0, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
Attr.QueueOrder[1] = JqQueueOrder{ 2, { 1, 2, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
Attr.WorkerOrderIndex[0] = 0;
Attr.WorkerOrderIndex[1] = 0;
Attr.WorkerOrderIndex[2] = 0;
Attr.WorkerOrderIndex[3] = 1;
JqStart(&Attr);
```

* `Flags`: can be set to `JQ_INIT_USE_SEPERATE_STACK`, to make Jq use a separate stack. This uses code from boost::context to seperately allocate a stack and use that. If it is not set, the calling stack will just be used.
* `NumWorkers`: Number of worker threads to start
* `StackSizeSmall`: Define the small stack size, ie the stacksize of jobs with Job Flag `JQ_JOBFLAG_SMALL_STACK` set. Only used when running with `JQ_INIT_USE_SEPERATE_STACK`.
* `StackSizeLarge`: Define the large(default) stack size. Only used when running with `JQ_INIT_USE_SEPERATE_STACK`.
* `NumQueueOrders`: Number of different QueueOrders in QueueOrder array.
* `WorkerOrderIndex`: For each worker thread, pick a queue order in QueueOrder Array
* `WorkerThreadAffinity`: Thread affinity to set for each worker thread. Not supported on osx.

## Non-Worker Threads executing jobs.

When Non-Worker threads execute jobs, they use the default ordering of the queues. To override this call
`JqSetThreadQueueOrder` to override it. It needs to be called for each thread waiting/executing jobs.


# Preconditions & Dependencies

While the main idea of Jq is that jobs are added inline whenever its needed, it is sometimes useful to express more complicated dependencies.

Jobs can be created with a block counter.
* `JqCreateBlocked` Reserves a job with a block count of one. 

The Block count can be manipulated manually with
* `JqBlock` Increase the block count by one
* `JqRelease` Decrease the block count by one

Note that once the block count reaches zero, the job may start to execute, and thus it is no longer valid to increase the block count. Note that `JqCreateBlocked` is the _only_ way to create a job with non-zero block count, so all jobs using block count must start with `JqCreateBlocked`


* `JqAddBlocked` Decrease the block count by one, and set the paramters of the job to execute when the counter reaches zero
* `JqAddPrecondition(A, B)` Make B a precondition for `A`. `A` must have an non-zero block count. Internally this increases the block count of A by one, and makes B decrement it when finished
* `JqAddSuccessor(Name, Precond, Job)` Run Job 


Note that it is optional for a Blocked job to have an actual body: its fine to just Release all block counts, in which case the Job will finish without running and trigger its dependencies. This can be used to implement barriers:

```
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
```

# Configuration

In `jq.h` there is a number of defines that can be changed how jq works

* `JQ_JOB_BUFFER_SIZE_BITS`: No. of Job Headers to reserve. Max. no. of jobs will be 2^JQ_JOB_BUFFER_SIZE_BITS
* `JQ_DEFAULT_WAIT_TIME_US`: Default argument passed into JqWait.
* `JQ_CACHE_LINE_SIZE`: Used to a align variables, to help against false sharing. 
* `JQ_API`: Empty by default. Use if you want to use Jq from a .dll/.so
* `JQ_FUNCTION_SIZE`: Max size of JqFunction. Increase if you want to be able to capture more data in the lambdas
* `JQ_MAX_THREADS`: Maximum number of threads using Jq, including worker threads
* `JQ_DEFAULT_STACKSIZE_SMALL`: Small stack size when using separate stacks
* `JQ_DEFAULT_STACKSIZE_LARGE`: Large stack size when using separate stacks
* `JQ_CHILD_HANDLE_BUFFER_SIZE`: Size of internal buffer when searching for child jobs


# Debugging And Graph Dumps.

Calling `JqGraphDumpStart` will make Jq start logging its state into a buffer of the specified size. Once `JqGraphDumpEnd` is called, a graphwiz file will be written to disk, showing a graph of the jobs added and executed and their dependencies. Use `dot` [Graphviz](https://graphviz.org/) to convert the file to a graph: `dot -Tps filename.gv -o filename.ps`

`JqDump` can be called to dump the full state of the job queue. This is meant for internal debugging, but can be used to show which jobs have pending block counts.


# Implementation
Jq allocates a buffer with space for `2^JQ_JOB_BUFFER_SIZE_BITS` active Jobs. While it might seem like a limiting factor, the fact that Jq supports running a job many times means that in practice you never run out of job slots.

The Job handles returned directly indexes into a global job array, using the bottom `JQ_JOB_BUFFER_SIZE_BITS` bits. Each Job header stores the last Claimed/Started/Finished Index, so checking if a job is done can be trivially done by checking if the finish count exceeds the handle passed in.

Composition is done by counting how many jobs needs to finish. When starting a child job this is incremented, and once the child(or a regular job) finishes, it decrements the counter. Last job to decrement finalizes the job, and decrements the counter of the parent job(if there is one).

Adding a child job also adds itself to the parent's linked list of children - this is the only thing that requires locking. This linked list is only needed for when we want to execute only child jobs: We need some way of being able to find the child of a given parent. Locking here is a decent compromise, as its rarely a contended issue.


# License
Licensed using unlicense.org