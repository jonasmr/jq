# JQ

JQ is a minimal job queue, that can be used to multithread work across multiple cpus. 
It contains a configurable priority system that can be used to control which jobs runs on what hardware threads.
The main implemention uses a single mutex to control access to jobs - This puts a limit to how well this system scales - However in practice it works pretty well. 
Finally there is an experimental lockless verision.

* github: https://github.com/jonasmr/jq.git

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

# License
Licensed using unlicense.org