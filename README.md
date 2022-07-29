# Jq

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


Adding a job is done by calling `JqAdd`. `JqAdd` returns a `JqHandle` handle, which can be passed to JqWait to wait for jobs to finish. `JqHandle` is a thin wrapper containing a `uint64`
```
	int Queue = 0;
	JqHandle JobHandle = JqAdd([]
	{
		printf("test\n");
	}, Queue);
	JqWait(JobHandle);

```

`JqAdd` Optionally takes a number of times to invoke job, and a range to split between the jobs:

```
	int Queue = 0;
	JqAdd([](int begin, int end)
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
	JqHandle H = JqAdd([=]
	{
		print("parent"\n");
		JqAdd([],
		{
			printf("child\n");
		}, queue);
	}, PipeId);
	JqWait(H);
	print("done\n");
```

Will print 
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


## Job Groups

Jq supports adding groups of jobs. This is effectively a job that is never run, but is only used to group child jobs so they can be waited for together. This is useful when you have a large body of code adding a variable amount of work, to which you want to be able to wait for at a later point.

```
	JqHandle H = JqGroupBegin();
	JqAdd([] {printf("a\n"), 0);
	JqAdd([] {printf("b\n"), 0);
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
		Attr.WorkerOrderIndex[3] = `;
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

# Configuration



# History
The original implementation of Jq only uses a single mutex to protect all of Jq. While this might seem like a inferior choice, the code shipped in both Hitman(2016) and Hitman 2, and the single lock was never an issue.
This can be found at the branch jq1-g2.
The Jq2 branch has a more modern implementation: the job queues are lockless, and a lock is only take whenever a job is finalized - and those locks are unique to the job slot, meaning contention is much less likely to occur.

# Implementation
# Functions

* `JqStart`: Start Jq.
* `JqStop`: Stop Jq.
* `JqAdd`: Add a Job.
* `JqWait`: Wait for a job.
* `JqWaitAll`: Wait for all jobs to finish.
* `JqExecuteOne`: can be called from any thread to execute a job. 
* `JqExecuteChildren`: Execute one child job of job passed in.
* `JqSetThreadPipeConfig`: can be called to set how non worker threads select jobs, when fetching jobs through `JqExecuteOne` and `JqWait`
* `JqSpawn`: Adds a job and immediately waits for it.
* `JqCancel`: Attempt to cancel a job. Fails if started or finished. Note that the only way to find out if a job is cancelled is by the return value of this function.
* `JqConsumeStats`: Consume various internal stats.

# Usage

In `jq.h` there is a number of defines that can be changed how jq works

* `JQ_PIPE_BUFFER_SIZE`: Size of job array. should at least be 2x the number of maximum active jobs
* `JQ_DEFAULT_WAIT_TIME_US`: Default argument passed into JqWait.
* `JQ_CACHE_LINE_SIZE`: Used to a align variables, to help against false sharing. 
* `JQ_API`: Empty by default. Use if you want to use Jq from a .dll/.so
* `JQ_FUNCTION_SIZE`: Max size of JqFunction. Increase if you want to be able to capture more data in the lambdas
* `JQ_NUM_PIPES`: Number of job pipes
* `JQ_MAX_THREADS`: Maximum number of threads using Jq, including worker threads
* `JQ_DEFAULT_STACKSIZE_SMALL`: Small stack size when using separate stacks
* `JQ_DEFAULT_STACKSIZE_LARGE`: Large stack size when using separate stacks


Jq is initialized by Iinitializing a JqAttr struct and calling JqStart

```
	JqAttributes Attr;
	JqInitAttributes(&Attr, NUMBER_OF_WORKER_THREADS);
	JqStart(&Attr);
```

`JqAttributes` contain members that can be used to control how Jq operates

```
	Attr.Flags = ...;
	Attr.ThreadConfig[0] = JqThreadConfig{ 7, {0, 1, 2, 3, 4, 5, 6, 0xff} };
	Attr.ThreadConfig[1] = JqThreadConfig{ 3, {3, 2, 1, 0xff, 0xff, 0xff, 0xff, 0xff} };
	Attr.ThreadConfig[2] = JqThreadConfig{ 2, {5, 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
	Attr.ThreadConfig[3] = JqThreadConfig{ 2, {1, 5, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff} };
	Attr.ThreadConfig[4] = JqThreadConfig{ 1, {7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,} };
```	

`Flags` can be set to `JQ_INIT_USE_SEPERATE_STACK`, to make Jq use a separate stack. If it is not set, the calling stack will just be used.
`ThreadConfig`: For each thread, contains a list of the pipes that thread will take jobs from


Shutting down waits for all jobs to finish:

```
	JqStop();
```


Adding a job is done by calling `JqAdd`. `JqAdd` returns a uint64 handle, which can be passed to JqWait to wait for jobs to finish
```
	int PipeId = 0;
	uint64_t JobHandle = JqAdd([]
	{
		printf("test\n");
	}, PipeId);
	JqWait(JobHandle);

```

`JqAdd` Optionally takes a number of times to invoke job, and a range to split between the jobs:

```
	int PipeId = 0;
	JqAdd([](int begin, int end)
	{
		printf("range %d %d\n", begin, end);
	}, PipeId, 2, 100);
```
This will invoke the printf job twice, passing in begin/end range such that the entire range (0-100) is covered
IE it might print
range 0 50
range 50 100


`JqWait` takes as argument some flags that controls two things
* How to find jobs when the job we are waiting for is not finished
	* `JQ_WAITFLAG_EXECUTE_SUCCESSORS`: Only execute jobs that are children of the current job
	* `JQ_WAITFLAG_EXECUTE_PREFER_SUCCESSORS`: Prefer child jobs, but allow other jobs to run in case of no child jobs available
	* `JQ_WAITFLAG_EXECUTE_ANY`: Execute any job
* What to do when the jobs is not done, and there isnt an available candidate job to run instead
	* `JQ_WAITFLAG_BLOCK`: Wait on a semaphore which is signalled by job when done
	* `JQ_WAITFLAG_SLEEP`: Sleep for the amount specified when calling
	* `JQ_WAITFLAG_SPIN`: Spin untill done.



# Child Jobs

By Default, a job added from another job is a child of that job. A parent job is not considered finished untill all children are finished.

```
	int PipeId = 0;
	uint64_t H = JqAdd([=]
	{
		print("parent"\n");
		JqAdd([],
		{
			printf("child\n");
		}, PipeId);
	}, PipeId);
	JqWait(H);
	print("done\n");
```

Is guaranteed to print
```
parent
child
done
```

A child job can be created with flag `JQ_JOBFLAG_DETACHED`, in which case the example above would not wait for the print of 'child'.

# Job Groups

Jq supports adding groups of jobs. This is effectively a job that is never run, but is only used to group child jobs so they can be waited for together.

```
	uint64_t H = JqGroupBegin();
	JqAdd([] {printf("a\n"), 0);
	JqAdd([] {printf("b\n"), 0);
	JqGroupEnd();
	JqWait(H); // will wait for print of a and b both
```

#Lockless Jq
Jq contains two implementations, one in `jqlockless.cpp` and one in `jqlocked.cpp`. The lockeless one should be considered experimental. 

#JqNode
If you prefer adding your jobs as nodes declared via objects, you can use JqNode, which is a wrapper on top of Jq.


#demo programs

* `demo_cancel.cpp`: demonstrate and test JqCancel
* `demo_priority.cpp`: demonstrate and test priority system
* `demo_stress.cpp`: stress test of how many jobs can be added and executed.
* `demo_node.cpp`: demostrate and test JqNode

# demo
# License
Licensed using unlicense.org