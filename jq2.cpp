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
//
// ***********************************************************************
//

//
//  TODO:
// 		* colors
//		* fix manymutex lock (lockless take-job)
//		* multidep
//		* peeking
//		* child-wait
//		* pop any job
//		* cancel job
//		* wait all
// 		run sanitizers
//		review wait child.
//		comment / code pass
// 		* fix spawn to always take one
//		* make handles
//		* futex for semaphore
//		one more stab at lockless queue?
//		verify the handle on trypopjob
//
// 		cleanup demos
//		delete old version
//		upgrade microprofile
//		* switch to ng
//		new doc
//		test win32
//		fix so there is only one queue impl.
//		test spawn
//		test wait for only children
// 		* fix unlocking on the wrong mutex when doing full wait in JqWait
//		* assert in ConditionVariable wait that the mutex passed in is actually locked
//
// 		make bench test a bit more.
//
// Flow
//
// Add:
//	* Allocate header (inc counter)
//	* Update header as reserved
//	* Fill in header
//  * if there is a parent, increment its pending count by no. of jobs
//	* lock pipe mtx
//  	* add to pipe
// 	* unlock
//
// Take job
// 	* lock pipe mtx
//		* decrement front jobs pending_start count
//		* if 0 flip remove from pending
//  * unlock mutex
//
//
// Finish:
//	* Decrement pending count
//	* if 0, tag as finished
//	* decrement parents pending count
//	* lock pipe mtx
//
// Reserve
//	* Allocate a header, set its preq count to 1
//
// Release Reserved  (can be used to do barriers)
// 	* decrement preq count by 1
//
//
// Add Dependency
// 	* increment preq count on a reserved header
//	* Cannot be done on a started job(IE must use reserve)
//
// Execute Children
//
//

#define JQ_IMPL
#include "jq.h"
#include "jqinternal.h"

#define JQ_JOBFLAG_UNINITIALIZED 0x80
#define JQ_MAX_SEMAPHORES JQ_MAX_THREADS
#define JQ_NUM_LOCKS 2048

#define JQ_QUEUE_FULL_EXECUTE_JOBS 1 // set to 1 to execute jobs when we're close to running out of handles
#define JQ_JOB_FILL_PRC_LIMIT 95	 // percentages of fullness to accept

#define JQ_JOBFLAG_EXTERNAL_MASK 0x3f
#define JQ_JOBFLAG_INTERNAL_SPAWN 0x40

// Split the finish counter into three 16bit parts
// [32]    is used to lock
// [16-31] is used to count children
// [0-15]  is used to count actual jobs
#define JOB_FINISH_LOCK (0x100000000llu)
#define JOB_FINISH_CHILD (0x000010000llu)
#define JOB_FINISH_CHILD_MASK (0x0ffff0000llu)
#define JOB_FINISH_PLAIN (0x000000001llu)
#define JOB_FINISH_PLAIN_MASK (0x00000ffffllu)

static_assert(JQ_JOB_BUFFER_SIZE == (1llu << JQ_JOB_BUFFER_SHIFT), "JQ_JOB_BUFFER_SIZE and JQ_JOB_BUFFER_SHIFT must match");
static_assert(JQ_NUM_QUEUES <= 64, "Currently a queue mask is being put in a uint64_t");

struct JqMutexLock;
enum JqWorkerState : uint8_t;
enum JqDebugStackState : uint8_t;

void				 JqWorker(int ThreadId);
void				 JqQueuePush(uint8_t Queue, uint64_t Handle);
uint16_t			 JqQueuePop(uint8_t Queue, uint16_t* OutSubJob);
bool				 JqQueueEmpty(uint8_t Queue);
bool				 JqPendingJobs(uint64_t Job);
void				 JqSelfPush(uint64_t Job, uint32_t JobIndex);
void				 JqSelfPop(uint64_t Job);
void				 JqFinishSubJob(uint16_t JobIndex, uint64_t Count);
void				 JqFinishInternal(uint16_t JobIndex);
void				 JqDecPrecondtion(uint64_t Handle, int Count, uint64_t* QueueTriggerMask = 0);
void				 JqIncPrecondtion(uint64_t Handle, int Count);
void				 JqUnpackQueueLink(uint64_t Value, uint16_t& Head, uint16_t& Tail, uint16_t& JobCount);
uint64_t			 JqPackQueueLink(uint16_t Head, uint16_t Tail, uint16_t JobCount);
void				 JqUnpackStartAndQueue(uint64_t Value, uint16_t& PendingStart, uint8_t& Queue);
uint64_t			 JqPackStartAndQueue(uint16_t PendingStart, uint8_t Queue);
JqMutex&			 JqGetQueueMutex(uint64_t QueueIndex);
JqMutex&			 JqGetJobMutex(uint64_t JobIndex);
JqConditionVariable& JqGetJobConditionVariable(uint64_t Handle);
uint16_t			 JqDependentJobLinkAlloc(uint64_t Handle);
void				 JqDependentJobLinkFreeList(uint16_t Index);
uint16_t			 JqQueuePopInternal(uint16_t JobIndex, uint8_t QueueIndex, uint16_t* OutSubJob, uint16_t* OutNextJob, bool PopAll);
void				 JqTriggerQueues(uint64_t QueueTriggerMask);
JqHandle			 JqAddInternal(JqHandle ReservedHandle, JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags, JqHandle PreconditionHandle);
void				 JqRunInternal(JqFunction* Function, int Begin, int End, uint32_t JobFlags);
void				 JqRunInternal(uint32_t WorkIndex, int Begin, int End);
bool				 JqTryPopJob(uint16_t JobIndex, uint16_t* OutSubJob, bool& OutIsDrained);
const char*			 JqWorkerStateString(JqWorkerState State);
const char*			 JqDebugStackStateString(JqDebugStackState State);
void				 JqDumpState();

struct JqSelfStack
{
	uint64_t Job;
	uint32_t JobIndex;
};
enum JqWorkerState : uint8_t
{
	JWS_NOT_WORKER,
	JWS_WORKING,
	JWS_IDLE,
};
enum JqDebugStackState : uint8_t
{
	JDS_EXECUTE,
	JDS_WAIT,
	JDS_WAIT_ALL,
	JDS_INVALID,
};

struct JqDebugState
{
	uint64_t		  Handle;
	uint32_t		  Flags;
	JqDebugStackState State;
	uint16_t		  SubIndex;
};
#define JQ_MAX_DEBUG_STACK (3 * JQ_MAX_JOB_STACK)
struct JqThreadState
{
	JqSelfStack SelfStack[JQ_MAX_JOB_STACK];
	uint32_t	SelfPos;
	uint32_t	HasLock;

	uint64_t ThreadId;

	uint32_t	  Initialized;
	JqWorkerState WorkerState;

	JqDebugState   DebugStack[JQ_MAX_JOB_STACK];
	uint32_t	   DebugPos;
	JqMutex**	   SingleMutexPtr;
	uint32_t*	   pJqNumQueues;
	uint8_t*	   pJqQueues;
	JqThreadState* NextThreadState;
};

JQ_THREAD_LOCAL JqThreadState ThreadState;
JqMutex						  ThreadStateLock;
JqThreadState*				  FirstThreadState = nullptr;
JqThreadState&				  JqGetThreadState();

struct JqDebugStackScope
{
	JqDebugStackScope(JqDebugStackState StackState, uint64_t Handle, uint32_t Flags, uint16_t SubIndex = 0)
	{
		JqThreadState& State = JqGetThreadState();
		JQ_ASSERT(State.DebugPos < JQ_MAX_DEBUG_STACK);
		JqDebugState& DebugState = State.DebugStack[State.DebugPos++];
		DebugState.State		 = StackState;
		DebugState.Flags		 = Flags;
		DebugState.Handle		 = Handle;
		DebugState.SubIndex		 = SubIndex;
	}
	~JqDebugStackScope()
	{
		JqThreadState& State = JqGetThreadState();
		JQ_ASSERT(State.DebugPos > 0);
		State.DebugPos--;
	}
};

#define JQ_TOKEN_PASTE0(a, b) a##b
#define JQ_TOKEN_PASTE(a, b) JQ_TOKEN_PASTE0(a, b)

#define JQ_DEBUG_SCOPE(State, Handle, Flags, SubIndex) JqDebugStackScope JQ_TOKEN_PASTE(jq_debug, __LINE__) = JqDebugStackScope(State, Handle, Flags, SubIndex)
//#define JQ_DEBUG_SCOPE(State, Handle, Flags) JQ_DEBUG_SCOPE(State, Handle, Flags, 0)

// linked list structure, for when jobs have multiple jobs that depend on them
struct JqDependentJobLink
{
	uint64_t Job;
	uint16_t Next;
	uint64_t Owner;
};

struct JqJob
{
	JqFunction Function;

	std::atomic<uint64_t> StartedHandle;  /// Handle which has been added to the queue
	std::atomic<uint64_t> FinishedHandle; /// Handle which was last finished
	std::atomic<uint64_t> ClaimedHandle;  /// Handle which has claimed this header
	std::atomic<uint64_t> Cancel;		  /// Largest Cancelled Handle

	std::atomic<uint64_t> PendingFinish;	 /// No. of jobs & (direct) child jobs that need to finish in order for this to be finished.
	std::atomic<uint64_t> PendingStart;		 /// No. of Jobs that needs to be Started, and the queue which is it inserted to
	std::atomic<uint8_t>  ActiveQueue;		 // only for debugging
	std::atomic<uint64_t> PreconditionCount; /// No. of Preconditions that need to finish, before this can be enqueued.
	JqDependentJobLink	  DependentJob;		 /// Job that is dependent on this job finishing.

	uint16_t NumJobs;		 /// Num Jobs to Finish
	uint16_t NumJobsToStart; /// Num Jobs to Start
	uint64_t Range;			 /// Range to pass to jobs
	uint32_t JobFlags;		 /// Job Flags
	uint8_t	 Queue;			 /// Priority of the job
	uint8_t	 Waiters;		 /// Set when waiting
	uint8_t	 WaitersWas;	 /// Prev wait flag(debug only)

	/// mutex protected.
	uint16_t Parent;
	uint16_t NextSibling;
	uint16_t PrevSibling;

	uint16_t FirstChild;
	uint16_t LastChild;

	bool Reserved;
};

#ifndef _WIN32
#include <pthread.h>
#endif
#include <atomic>

#ifdef _WIN32
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

#if 0
#define llqprintf(...) printf(__VA_ARGS__);
#else
#define llqprintf(...)                                                                                                                                                                                 \
	do                                                                                                                                                                                                 \
	{                                                                                                                                                                                                  \
	} while(0)
#endif

// lockless queue..
struct JqLocklessQueue
{
	struct Entry
	{
		// Packed
		// [48-63] popped sequence number.
		// [32-47] pushed sequence number.
		// [0-31] payload

		std::atomic<uint64_t> Entry;
		uint64_t			  pad[JQ_CACHE_LINE_SIZE / sizeof(uint64_t) - sizeof(uint64_t)];
	};
	static constexpr const uint32_t SEQUENCE_SHIFT = 11;
	static constexpr const uint32_t BUFFER_SIZE	   = 1 << SEQUENCE_SHIFT;

	Entry Entries[BUFFER_SIZE];

	std::atomic<uint64_t> PushPop;
	int					  Index;

	static uint64_t PackEntry(uint16_t PushSequence, uint16_t PopSequence, uint32_t Payload)
	{
		return ((uint64_t)PushSequence << 48llu) | ((uint64_t)PopSequence << 32llu) | Payload;
	}
	static void UnpackEntry(uint64_t Packed, uint16_t& PushSequence, uint16_t& PopSequence, uint32_t& Payload)
	{
		PushSequence = (uint16_t)(Packed >> 48llu);
		PopSequence	 = (uint16_t)(Packed >> 32llu);
		Payload		 = (uint32_t)(Packed & 0xffffffff);
	}

	static uint64_t PackPushPop(uint32_t Push, uint32_t Pop)
	{
		return ((uint64_t)Push << 32llu) | (uint64_t)Pop;
	}
	static void UnpackPushPop(uint64_t Packed, uint32_t& Push, uint32_t& Pop)
	{
		Push = (uint32_t)(Packed >> 32llu);
		Pop	 = (uint32_t)(Packed & 0xffffffff);
	}

	void Init(int Index)
	{
		this->Index = Index;
		PushPop.store(0);
		for(Entry& e : Entries)
		{

			uint64_t Value = PackEntry(0xffff, 0xffff, 0);
			e.Entry.store(Value);
		}
		llqprintf("init done %p\n", this);
		static_assert(BUFFER_SIZE == JQ_JOB_BUFFER_SIZE, "BUFFER_SIZE must match JQ_JOB_BUFFER_SIZE");
	}

	bool Peek(uint32_t& PeekOut, uint32_t* Ref)
	{
		uint32_t Push, Pop, Payload;
		uint64_t Old, New, Entry;
		uint16_t PushSequence, PopSequence;
		do
		{
			Old = PushPop.load();
			UnpackPushPop(Old, Push, Pop);
			if(Push == Pop)
				return false;

			uint32_t PopIndex	  = Pop & (BUFFER_SIZE - 1);
			uint16_t Sequence	  = Pop >> SEQUENCE_SHIFT;
			uint16_t PrevSequence = Sequence - 1;

			Entry = Entries[PopIndex].Entry.load();
			UnpackEntry(Entry, PushSequence, PopSequence, Payload);
			if(PushSequence != Sequence)
			{
				// not done pushing, so we have to back off.
			}
			else if(PrevSequence == PopSequence)
			{
				if(Ref)
					*Ref = Pop;
				PeekOut = Payload;
				if(Payload == 0)
				{
					JqDumpState();
				}
				JQ_ASSERT(Payload != 0);
				return true;
			}
			// if sequence doesn't match, someone popped inbetween, so the payload should be zero, and we have to retry

			// backoff here
		} while(true);
	}
	// pops a value.
	// optionally only pops if that value has sequence given as arg
	bool Pop(uint32_t& PoppedValue, uint32_t* Ref)
	{
		uint32_t Push, Pop, Payload;
		uint64_t Old, New, Entry, NewEntry;
		uint16_t PushSequence, PopSequence;
		bool	 UseRef	  = Ref != 0;
		uint32_t RefValue = Ref ? *Ref : 0;
		do
		{
			Old = PushPop.load();
			UnpackPushPop(Old, Push, Pop);
			if(Push == Pop)
				return false; // Queue empty

			if(UseRef && Pop != RefValue) // something else got popped.
				return false;

			uint32_t PopIndex	  = Pop & (BUFFER_SIZE - 1);
			uint16_t Sequence	  = Pop >> SEQUENCE_SHIFT;
			uint16_t PrevSequence = Sequence - 1;
			Entry				  = Entries[PopIndex].Entry.load();
			UnpackEntry(Entry, PushSequence, PopSequence, Payload);
			if(PushSequence != Sequence)
			{
				// someones not done pushing. can't push yet..
				// back off
			}
			else if(PrevSequence == PopSequence)
			{
				// ready to pop
				New = PackPushPop(Push, Pop + 1);
				if(PushPop.compare_exchange_weak(Old, New))
				{
					PoppedValue = Payload;
					// we're done updating push/pop, now clear the payload.
					Entries[PopIndex].Entry.store(PackEntry(PushSequence, Sequence, 0));
					llqprintf("popped %d [%d/%d] %8d :: %8d %8d   [%lld/%lld]\n", Index, Push, Pop, Payload, PopIndex, Sequence, PushPop.load() >> 32, PushPop.load() & 0xffffffff);
					return true;
				}
			}
			// backoff here
		} while(true);
	}

	void Push(uint32_t Value)
	{
		if(Value == 0)
			JQ_BREAK();
		// queue should never be fucking full.
		uint32_t Push, Pop, Payload;
		uint64_t Old, New;
		uint16_t PushSequence, PopSequence;

		do
		{
			Old = PushPop.load();
			UnpackPushPop(Old, Push, Pop);
			if(((Push + 1) % JQ_JOB_BUFFER_SIZE) == (Pop % JQ_JOB_BUFFER_SIZE))
			{
				// queue is full. this should never occur, calling code should back off or crash
				// intentionally not supported.
				JQ_BREAK();
			}
			New = PackPushPop(Push + 1, Pop);

			if(PushPop.compare_exchange_weak(Old, New))
				break;
			// exp backoff here

		} while(true);

		// now commit the value in the queue
		uint32_t PushIndex	  = Push & (BUFFER_SIZE - 1);
		uint16_t Sequence	  = Push >> SEQUENCE_SHIFT;
		uint16_t PrevSequence = Sequence - 1;
		llqprintf("pushin %d [%d/%d]%8d :: %8d %8d  [%lld/%lld]\n", Index, Push, Pop, Value, PushIndex, Sequence, PushPop.load() >> 32, PushPop.load() & 0xffffffff);

		// atomically insert and mark the value.
		// do backoff, if for some reason the value isn't popped
		do
		{
			Old = Entries[PushIndex].Entry.load();
			UnpackEntry(Old, PushSequence, PopSequence, Payload);

			// handle the case where a poppin' hasnt committed its pop
			if(PopSequence != PrevSequence)
			{
				// JQ_BREAK(); // do exp. backoff?
			}
			if(PushSequence != PrevSequence)
			{
				// JQ_BREAK();
			}
			JQ_ASSERT(Payload == 0);

			New = PackEntry(Sequence, PopSequence, Value);

			if(Entries[PushIndex].Entry.compare_exchange_weak(Old, New))
				break;
		} while(true);
	}
	template <typename T>
	void DebugCallbackAll(T Function)
	{
		uint32_t Push, Pop;
		UnpackPushPop(PushPop.load(), Push, Pop);
		while(Pop != Push)
		{
			uint32_t PopIndex = Pop & (BUFFER_SIZE - 1);
			uint16_t Sequence = Pop >> SEQUENCE_SHIFT;

			uint64_t Entry = Entries[PopIndex].Entry.load();
			uint16_t EntryPushSequence, EntryPopSequence;
			uint32_t Payload;
			UnpackEntry(Entry, EntryPushSequence, EntryPopSequence, Payload);

			Function(Pop, PopIndex, Sequence, EntryPushSequence, EntryPopSequence, Payload);
			Pop++;
		}
	}
};

struct JQ_ALIGN_CACHELINE JqState_t
{
	JqSemaphore	 Semaphore[JQ_MAX_SEMAPHORES];
	uint64_t	 SemaphoreMask[JQ_MAX_SEMAPHORES];
	uint8_t		 QueueNumSemaphores[JQ_NUM_QUEUES];
	uint8_t		 QueueToSemaphore[JQ_NUM_QUEUES][JQ_MAX_SEMAPHORES];
	uint8_t		 SemaphoreClients[JQ_MAX_SEMAPHORES][JQ_MAX_THREADS];
	uint8_t		 SemaphoreClientCount[JQ_MAX_SEMAPHORES];
	int			 ActiveSemaphores;
	JqAttributes Attributes;
	uint8_t		 NumQueues[JQ_MAX_THREADS];
	uint8_t		 QueueList[JQ_MAX_THREADS][JQ_NUM_QUEUES];
	uint8_t		 SemaphoreIndex[JQ_MAX_THREADS];
	JQ_THREAD	 WorkerThreads[JQ_MAX_THREADS];

	JqJobStackList StackSmall;
	JqJobStackList StackLarge;
	int			   NumWorkers;
	int			   Stop;
	int			   TotalWaiting;

	std::atomic<uint32_t> ActiveJobs;
	std::atomic<uint64_t> NextHandle;

	JqQueueOrder ThreadConfig[JQ_MAX_THREADS];
	JqJob		 Jobs[JQ_JOB_BUFFER_SIZE];

	JqLocklessQueue LocklessQueues[JQ_NUM_QUEUES];

	JqMutex				MutexJob[JQ_NUM_LOCKS];
	JqConditionVariable ConditionVariableJob[JQ_NUM_LOCKS];

	JqMutex				  DependentJobLinkMutex;
	JqDependentJobLink	  DependentJobLinks[JQ_JOB_BUFFER_SIZE];
	uint16_t			  DependentJobLinkHead;
	std::atomic<uint32_t> DependentJobLinkCounter;

	JqState_t()
		: NumWorkers(0)
	{
	}

	JqStats Stats;
} JqState;

#ifdef _WIN32
#pragma warning(pop)
#endif

JQ_THREAD_LOCAL int		 JqSpinloop				   = 0; // prevent optimizer from removing spin loop
JQ_THREAD_LOCAL uint32_t g_nJqNumQueues			   = 0;
JQ_THREAD_LOCAL uint8_t	 g_JqQueues[JQ_NUM_QUEUES] = { 0 };
JQ_THREAD_LOCAL JqJobStack* g_pJqJobStacks		   = 0;

void JqStart(JqAttributes* pAttr)
{
#if 0
	//verify macros
	uint64_t t0 = 0xf000000000000000;
	uint64_t t1 = 0;
	uint64_t t2 = 0x1000000000000000;
	JQ_ASSERT(JQ_LE_WRAP(t0, t0));
	JQ_ASSERT(JQ_LE_WRAP(t1, t1));
	JQ_ASSERT(JQ_LE_WRAP(t2, t2));
	JQ_ASSERT(JQ_LE_WRAP(t0, t1));
	JQ_ASSERT(!JQ_LE_WRAP(t1, t0));
	JQ_ASSERT(JQ_LE_WRAP(t1, t2));
	JQ_ASSERT(!JQ_LE_WRAP(t2, t1));
	JQ_ASSERT(JQ_LE_WRAP(t0, t2));
	JQ_ASSERT(!JQ_LE_WRAP(t2, t0));

	JQ_ASSERT(!JQ_LT_WRAP(t0, t0));
	JQ_ASSERT(!JQ_LT_WRAP(t1, t1));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t2));
	JQ_ASSERT(JQ_LT_WRAP(t0, t1));
	JQ_ASSERT(!JQ_LT_WRAP(t1, t0));
	JQ_ASSERT(JQ_LT_WRAP(t1, t2));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t1));
	JQ_ASSERT(JQ_LT_WRAP(t0, t2));
	JQ_ASSERT(!JQ_LT_WRAP(t2, t0));
#endif

	JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & (uint64_t)&JqState) == 0);
	// JQ_ASSERT(((JQ_CACHE_LINE_SIZE - 1) & offsetof(JqState_t, Mutex)) == 0);
	JQ_ASSERT(JqState.NumWorkers == 0);

	memset(JqState.QueueList, 0xff, sizeof(JqState.QueueList));
	memset(JqState.NumQueues, 0, sizeof(JqState.NumQueues));
	memset(JqState.SemaphoreMask, 0, sizeof(JqState.SemaphoreMask));
	memset(JqState.QueueNumSemaphores, 0, sizeof(JqState.QueueNumSemaphores));
	memset(JqState.QueueToSemaphore, 0, sizeof(JqState.QueueToSemaphore));
	memset(JqState.SemaphoreClients, 0, sizeof(JqState.SemaphoreClients));
	memset(JqState.SemaphoreClientCount, 0, sizeof(JqState.SemaphoreClientCount));
	JqState.ActiveSemaphores = 0;
	JqState.Attributes		 = *pAttr;
	JqState.NumWorkers		 = pAttr->NumWorkers;

	for(uint32_t i = 0; i < pAttr->NumWorkers; ++i)
	{
		JQ_ASSERT(pAttr->WorkerOrderIndex[i] < pAttr->NumQueueOrders); /// out of bounds pipe order index in attributes
		JQ_ASSERT(pAttr->WorkerOrderIndex[i] < JQ_NUM_QUEUES);
		JqState.ThreadConfig[i] = pAttr->QueueOrder[pAttr->WorkerOrderIndex[i]];
	}

	for(int i = 0; i < JQ_NUM_QUEUES; ++i)
	{
		JqState.LocklessQueues[i].Init(i);
	}

	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JqQueueOrder& C				  = JqState.ThreadConfig[i];
		uint8_t		  nNumActivePipes = 0;
		uint64_t	  PipeMask		  = 0;
		static_assert(JQ_NUM_QUEUES < 64, "wont fit in 64bit mask");
		for(uint32_t j = 0; j < C.nNumPipes; ++j)
		{
			if(C.Queues[j] != 0xff)
			{
				JqState.QueueList[i][nNumActivePipes++] = C.Queues[j];
				JQ_ASSERT(C.Queues[j] < JQ_NUM_QUEUES);
				PipeMask |= 1llu << C.Queues[j];
			}
		}
		JQ_ASSERT(nNumActivePipes); // worker without active pipes.
		JqState.NumQueues[i]   = nNumActivePipes;
		int nSelectedSemaphore = -1;
		for(int j = 0; j < JqState.ActiveSemaphores; ++j)
		{
			if(JqState.SemaphoreMask[j] == PipeMask)
			{
				nSelectedSemaphore = j;
				break;
			}
		}
		if(-1 == nSelectedSemaphore)
		{
			JQ_ASSERT(JqState.ActiveSemaphores < JQ_MAX_SEMAPHORES);
			nSelectedSemaphore						  = JqState.ActiveSemaphores++;
			JqState.SemaphoreMask[nSelectedSemaphore] = PipeMask;
			for(uint32_t j = 0; j < JQ_NUM_QUEUES; ++j)
			{
				if(PipeMask & (1llu << j))
				{
					JQ_ASSERT(JqState.QueueNumSemaphores[j] < JQ_MAX_SEMAPHORES);
					JqState.QueueToSemaphore[j][JqState.QueueNumSemaphores[j]++] = (uint8_t)nSelectedSemaphore;
				}
			}
		}
		JQ_ASSERT(JqState.SemaphoreClientCount[nSelectedSemaphore] < JQ_MAX_SEMAPHORES);
		JqState.SemaphoreClients[nSelectedSemaphore][JqState.SemaphoreClientCount[nSelectedSemaphore]++] = (uint8_t)i;
		JqState.SemaphoreIndex[i]																		 = (uint8_t)nSelectedSemaphore;
	}

	for(uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		JqState.Semaphore[i].Init(JqState.SemaphoreClientCount[i] ? JqState.SemaphoreClientCount[i] : 1);
	}

#if 0
	for (uint32_t i = 0; i < JQ_NUM_QUEUES; ++i)
	{
		printf("pipe %d : ", i);
		for (uint32_t j = 0; j < JqState.QueueNumSemaphores[i]; ++j)
		{
			printf("%d, ", JqState.QueueToSemaphore[i][j]);
		}
		printf("\n");
	}


	for (uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		if (JqState.SemaphoreClientCount[i])
		{
			printf("Semaphore %d, clients %d Mask %llx :: ", i, JqState.SemaphoreClientCount[i], JqState.SemaphoreMask[i]);
			for (uint32_t j = 0; j < JqState.SemaphoreClientCount[i]; ++j)
			{
				printf("%d, ", JqState.SemaphoreClients[i][j]);
			}
			printf("\n");
		}
	}

	for (int i = 0; i < nNumWorkers; ++i)
	{
		printf("worker %d Sema %d Mask %llx :: Pipes ", i, JqState.SemaphoreIndex[i], JqState.SemaphoreMask[JqState.SemaphoreIndex[i]]);
		for (uint32_t j = 0; j < JqState.NumQueues[i]; ++j)
		{
			printf("%d, ", JqState.QueueList[i][j]);
		}
		printf("\n");

	}
#endif
	JqState.TotalWaiting = 0;
	JqState.Stop		 = 0;
	for(int i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		JqState.Jobs[i].StartedHandle  = 0;
		JqState.Jobs[i].FinishedHandle = 0;
	}
	// memset(&JqState.nPrioListHead, 0, sizeof(JqState.nPrioListHead));
	JqState.ActiveJobs			   = 0;
	JqState.NextHandle			   = 1;
	JqState.Stats.nNumFinished	   = 0;
	JqState.Stats.nNumLocks		   = 0;
	JqState.Stats.nNumSema		   = 0;
	JqState.Stats.nNumLocklessPops = 0;
	JqState.Stats.nNumWaitCond	   = 0;

	for(uint16_t i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		// terminate at end, and tag zero as unusable
		if(i == 0 || i == (JQ_JOB_BUFFER_SIZE - 1))
		{
			JqState.DependentJobLinks[i].Next = 0;
		}
		else
		{
			JqState.DependentJobLinks[i].Next = i + 1;
		}
		JqState.DependentJobLinks[i].Job = 0;
	}
	JqState.DependentJobLinkHead = 1;

	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JQ_THREAD_CREATE(&JqState.WorkerThreads[i]);
		JQ_THREAD_START(&JqState.WorkerThreads[i], JqWorker, (i));
	}
}
int JqNumWorkers()
{
	return JqState.NumWorkers;
}

void JqStop()
{
	JqWaitAll();
	JqState.Stop = 1;
	for(int i = 0; i < JqState.ActiveSemaphores; ++i)
	{
		JqState.Semaphore[i].Signal(JqState.NumWorkers);
	}
	for(int i = 0; i < JqState.NumWorkers; ++i)
	{
		JQ_THREAD_JOIN(&JqState.WorkerThreads[i]);
		JQ_THREAD_DESTROY(&JqState.WorkerThreads[i]);
	}
	JqFreeAllStacks(JqState.StackSmall);
	JqFreeAllStacks(JqState.StackLarge);

	JqState.NumWorkers = 0;
}

void JqSetThreadQueueOrder(JqQueueOrder* pConfig)
{
	JQ_ASSERT(g_nJqNumQueues == 0); // its not supported to change this value, nor is it supported to set it on worker threads. set on init instead.
	uint32_t nNumActiveQueues = 0;
	JQ_ASSERT(pConfig->nNumPipes <= JQ_NUM_QUEUES);
	for(uint32_t j = 0; j < pConfig->nNumPipes; ++j)
	{
		if(pConfig->Queues[j] != 0xff)
		{
			g_JqQueues[nNumActiveQueues++] = pConfig->Queues[j];
			JQ_ASSERT(pConfig->Queues[j] < JQ_NUM_QUEUES);
		}
	}
	g_nJqNumQueues = nNumActiveQueues;
}

void JqConsumeStats(JqStats* pStats)
{
	*pStats						   = JqState.Stats;
	JqState.Stats.nNumAdded		   = 0;
	JqState.Stats.nNumFinished	   = 0;
	JqState.Stats.nNumAddedSub	   = 0;
	JqState.Stats.nNumFinishedSub  = 0;
	JqState.Stats.nNumCancelled	   = 0;
	JqState.Stats.nNumCancelledSub = 0;
	JqState.Stats.nNumLocks		   = 0;
	JqState.Stats.nNumSema		   = 0;
	JqState.Stats.nNumLocklessPops = 0;
	JqState.Stats.nNumWaitCond	   = 0;
	JqState.Stats.nNextHandle.H	   = JqState.NextHandle;
}

void JqFinishInternal(uint16_t JobIndex)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqFinishInternal", 0xffff);

	JobIndex	 = JobIndex % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job = JqState.Jobs[JobIndex];
	uint16_t PendingStart;
	uint8_t	 Queue;

	JQ_ASSERT(Job.PendingStart.load() == 0);
	JQ_ASSERT(Job.PendingFinish.load() == 0);
	// no need to lock queue, since we are removed a long time ago

	uint64_t		   FinishValue = 0;
	uint16_t		   Parent	   = 0;
	JqDependentJobLink Dependent;
	{
		JqSingleMutexLock L(JqGetJobMutex(JobIndex));

		Parent	   = Job.Parent;
		Job.Parent = 0;
		JQ_ASSERT(Job.FirstChild == 0);
		JQ_ASSERT(Job.LastChild == 0);
		// Siblings can only be cleared while parent lock is taken

		JQ_CLEAR_FUNCTION(Job.Function);

		Dependent			  = Job.DependentJob;
		Job.DependentJob.Job  = 0;
		Job.DependentJob.Next = 0;

		JqState.Stats.nNumFinished++;
		// kick waiting threads.
		int8_t Waiters = Job.Waiters;
		if(Waiters != 0)
		{
			JqState.Stats.nNumWaitKicks++;
			JqGetJobConditionVariable(JobIndex).NotifyAll();
			Job.Waiters	   = 0;
			Job.WaitersWas = Waiters;
		}
		else
		{
			Job.WaitersWas = 0xff;
		}
		JQ_ASSERT(Job.FinishedHandle.load() != Job.StartedHandle.load());
		JQ_ASSERT(Job.ClaimedHandle.load() == Job.StartedHandle.load());

		FinishValue = Job.StartedHandle.load();
	}
	if(Parent)
	{
		{
			JqSingleMutexLock L(JqGetJobMutex(Parent));
			JqJob&			  ParentJob = JqState.Jobs[JQ_GET_INDEX(Parent)];
			if(Job.NextSibling != 0)
			{
				JqState.Jobs[Job.NextSibling].PrevSibling = Job.PrevSibling;
				JQ_ASSERT(JobIndex != ParentJob.LastChild);
			}
			else
			{
				JQ_ASSERT(JobIndex == ParentJob.LastChild);
				ParentJob.LastChild = Job.PrevSibling;
			}
			if(Job.PrevSibling != 0)
			{
				JqState.Jobs[Job.PrevSibling].NextSibling = Job.NextSibling;
				JQ_ASSERT(JobIndex != ParentJob.FirstChild);
			}
			else
			{
				JQ_ASSERT(JobIndex == ParentJob.FirstChild);
				ParentJob.FirstChild = Job.NextSibling;
			}
		}
		JqFinishSubJob(Parent, JOB_FINISH_CHILD);
	}

	// this releases the header.
	Job.FinishedHandle = FinishValue;

	if(Dependent.Job)
	{
		JQ_MICROPROFILE_VERBOSE_SCOPE("DecPrecondtion", MP_AUTO);
		uint64_t QueueTriggerMask = 0;
		JqDecPrecondtion(Dependent.Job, 1, &QueueTriggerMask);
		uint16_t Next = Dependent.Next;
		while(Next)
		{
			uint64_t Job = JqState.DependentJobLinks[Next].Job;
			Next		 = JqState.DependentJobLinks[Next].Next;
			JqDecPrecondtion(Job, 1, &QueueTriggerMask);
		}
		// Trigger only once. the overhead for kicking all the queues can be substantial
		JqTriggerQueues(QueueTriggerMask);
	}
	if(Dependent.Next)
	{
		// First element is embedded so it doesn't need freeing.
		JqDependentJobLinkFreeList(Dependent.Next);
	}
}

void JqFinishSubJob(uint16_t nJobIndex, uint64_t FinishCount = 1)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqFinishSubJob", 0xffff);
	nJobIndex			 = nJobIndex % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job		 = JqState.Jobs[nJobIndex];
	uint64_t before		 = Job.PendingFinish.fetch_sub(FinishCount);
	uint64_t FinishIndex = before - FinishCount;
	JqState.Stats.nNumFinishedSub++;
	JQ_ASSERT((int64_t)FinishIndex >= 0);
	if(0 == FinishIndex)
	{
		JqFinishInternal(nJobIndex);
	}
}

void JqAttachChild(uint64_t Parent, uint64_t Child)
{
	if(!Parent)
		return;
	uint16_t ParentIndex = Parent % JQ_JOB_BUFFER_SIZE;
	uint16_t ChildIndex	 = Child % JQ_JOB_BUFFER_SIZE;
	JqJob&	 ParentJob	 = JqState.Jobs[ParentIndex];
	JqJob&	 ChildJob	 = JqState.Jobs[ChildIndex];

	JQ_MICROPROFILE_VERBOSE_SCOPE("JQ_ATTACH_CHILD", MP_BLACK);

	JqSingleMutexLock L(JqGetJobMutex(ParentIndex));

	// can't add parent/child relations to already finished jobs.
	JQ_ASSERT(ParentJob.FinishedHandle != Parent);
	JQ_ASSERT(ChildJob.FinishedHandle != Child);
	// handles must be claimed
	JQ_ASSERT(ParentJob.ClaimedHandle == Parent);
	JQ_ASSERT(ChildJob.ClaimedHandle == Child);

	JQ_ASSERT(ChildJob.Parent == 0);
	JQ_ASSERT(ChildJob.NextSibling == 0);
	JQ_ASSERT(ChildJob.PrevSibling == 0);
	ChildJob.Parent = ParentIndex;

	uint16_t LastChild	 = ParentJob.LastChild;
	ChildJob.PrevSibling = LastChild;

	if(LastChild)
	{
		JQ_ASSERT(JqState.Jobs[LastChild].NextSibling == 0);
		JqState.Jobs[LastChild].NextSibling = ChildIndex;
	}
	else
	{
		JQ_ASSERT(ParentJob.LastChild == ParentJob.FirstChild);
		ParentJob.FirstChild = ChildIndex;
	}

	ParentJob.LastChild = ChildIndex;

	uint64_t Before = ParentJob.PendingFinish.fetch_add(JOB_FINISH_CHILD);
	JQ_ASSERT(Before > 0);
}

// splits range evenly to jobs
int JqGetRangeStart(int nIndex, int nFraction, int nRemainder)
{
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

void JqSelfPush(uint64_t Job, uint32_t SubIndex)
{
	JqThreadState& State = JqGetThreadState();

	State.SelfStack[State.SelfPos].Job		= Job;
	State.SelfStack[State.SelfPos].JobIndex = SubIndex;
	State.SelfPos++;
}

void JqSelfPop(uint64_t Job)
{
	JqThreadState& State = JqGetThreadState();

	JQ_ASSERT(State.SelfPos != 0);
	State.SelfPos--;
	JQ_ASSERT(State.SelfStack[State.SelfPos].Job == Job);
}

uint32_t JqSelfJobIndex()
{
	JqThreadState& State = JqGetThreadState();

	JQ_ASSERT(State.SelfPos != 0);
	return State.SelfStack[State.SelfPos - 1].JobIndex;
}

int JqGetNumWorkers()
{
	return JqState.NumWorkers;
}

void JqContextRun(JqTransfer T)
{
	JqJobStack* pJobData = (JqJobStack*)T.data;
	(*pJobData->Function)(pJobData->Begin, pJobData->End);
	jq_jump_fcontext(T.fctx, (void*)447);
	JQ_BREAK();
}

JqJobStackList& JqGetJobStackList(uint32_t nFlags)
{
	bool bSmall = 0 != (nFlags & JQ_JOBFLAG_SMALL_STACK);
	return bSmall ? JqState.StackSmall : JqState.StackLarge;
}

void JqRunInternal(JqFunction* Function, int Begin, int End, uint32_t JobFlags)
{
	if(JQ_INIT_USE_SEPERATE_STACK == (JqState.Attributes.Flags & JQ_INIT_USE_SEPERATE_STACK))
	{
		bool		Small	  = 0 != (JobFlags & JQ_JOBFLAG_SMALL_STACK);
		uint32_t	StackSize = Small ? JqState.Attributes.StackSizeSmall : JqState.Attributes.StackSizeLarge;
		JqJobStack* JobData	  = JqAllocStack(JqGetJobStackList(JobFlags), StackSize, JobFlags);
		void*		Verify	  = g_pJqJobStacks;
		JQ_ASSERT(JobData->Link == nullptr);
		JobData->Link		= g_pJqJobStacks;
		JobData->Begin		= Begin;
		JobData->End		= End;
		JobData->Function	= Function;
		g_pJqJobStacks		= JobData;
		JobData->ContextJob = jq_make_fcontext(JobData->StackTop(), JobData->StackSize(), JqContextRun);
		JqTransfer T		= jq_jump_fcontext(JobData->ContextJob, (void*)JobData);
		JQ_ASSERT(T.data == (void*)447);
		g_pJqJobStacks = JobData->Link;
		JobData->Link  = nullptr;
		JQ_ASSERT(Verify == g_pJqJobStacks);
		JQ_ASSERT(JobData->GUARD[0] == 0xececececececececll);
		JQ_ASSERT(JobData->GUARD[1] == 0xececececececececll);
		JqFreeStack(JqGetJobStackList(JobFlags), JobData);
	}
	else
	{
		(*Function)(Begin, End);
	}
}
void JqRunInternal(uint32_t WorkIndex, int Begin, int End)
{
	JQ_ASSERT(JqState.Jobs[WorkIndex].PreconditionCount == 0);
	JqRunInternal(&JqState.Jobs[WorkIndex].Function, Begin, End, JqState.Jobs[WorkIndex].JobFlags);
}

void JqExecuteJob(uint64_t nJob, uint16_t nSubIndex)
{
	JQ_DEBUG_SCOPE(JDS_EXECUTE, nJob, 0, nSubIndex);

	JQ_MICROPROFILE_SCOPE("Execute", 0xc0c0c0);
	JqThreadState& State = JqGetThreadState();

	JQ_ASSERT(State.SelfPos < JQ_MAX_JOB_STACK);
	uint16_t nWorkIndex = nJob % JQ_JOB_BUFFER_SIZE;
	JQ_ASSERT(nWorkIndex);

	if(JqState.Jobs[nWorkIndex].Cancel.load() != nJob)
	{
		// Some basic assertions, which, if they don't hold true, we fucked up
		JQ_ASSERT(JqState.Jobs[nWorkIndex].StartedHandle.load() == JqState.Jobs[nWorkIndex].ClaimedHandle.load());
		JQ_ASSERT(JQ_LT_WRAP(JqState.Jobs[nWorkIndex].FinishedHandle.load(), JqState.Jobs[nWorkIndex].StartedHandle.load()));
		JQ_ASSERT(JqState.Jobs[JQ_GET_INDEX(nJob)].StartedHandle.load() != 0);
		//	JQ_ASSERT(JqState.Jobs[JQ_GET_INDEX(Work)].StartedHandle.load() == JqState.Jobs[JQ_GET_INDEX(Work)].ClaimedHandle.load());

		JqSelfPush(nJob, nSubIndex);
		{
			uint16_t nNumJobs	= JqState.Jobs[nWorkIndex].NumJobs;
			int		 nRange		= JqState.Jobs[nWorkIndex].Range;
			int		 nFraction	= nRange / nNumJobs;
			int		 nRemainder = nRange - nFraction * nNumJobs;
			int		 nStart		= JqGetRangeStart(nSubIndex, nFraction, nRemainder);
			int		 nEnd		= JqGetRangeStart(nSubIndex + 1, nFraction, nRemainder);
			JqRunInternal(nWorkIndex, nStart, nEnd);
		}
		JqSelfPop(nJob);
	}

	JqFinishSubJob(nJob, 1);
}

uint16_t JqTakeJob(uint16_t* pSubIndex, uint32_t nNumQueues, uint8_t* pQueues)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("JqTakeJob", MP_AUTO); // if this starts happening the job queue size should be increased..
	const uint32_t nCount = nNumQueues ? nNumQueues : JQ_NUM_QUEUES;
	for(uint32_t i = 0; i < nCount; i++)
	{
		uint8_t	 nQueue = nNumQueues ? pQueues[i] : i;
		uint16_t nIndex = JqQueuePop(nQueue, pSubIndex);
		JQ_ASSERT(JqState.Jobs[nIndex].StartedHandle.load() == JqState.Jobs[nIndex].ClaimedHandle.load());
		if(nIndex)
			return nIndex;
	}
	return 0;
}

bool JqTakeJobFromHandle(uint64_t Handle, uint16_t* SubIndexOut)
{
	JqHandle H = JqHandle{ Handle };
	if(!JqIsDone(H) && JqIsStarted(H))
	{
		uint16_t Index = JQ_GET_INDEX(Handle);
		JqJob&	 Job   = JqState.Jobs[Index];
		if(Job.StartedHandle.load() != Handle || Job.PendingStart.load() == 0)
			return false;
		bool IsDrained = false;
		return JqTryPopJob(Index, SubIndexOut, IsDrained);
	}
	return false;
}

bool JqIsStarted(JqHandle Handle)
{
	uint16_t Index		   = JQ_GET_INDEX(Handle.H);
	JqJob&	 Job		   = JqState.Jobs[Index];
	uint64_t StartedHandle = Job.StartedHandle;
	return JQ_LE_WRAP(Handle.H, StartedHandle);
}

uint16_t JqPendingStarts(uint64_t Handle)
{
	uint16_t Index		   = JQ_GET_INDEX(Handle);
	JqJob&	 Job		   = JqState.Jobs[Index];
	uint64_t StartedHandle = Job.StartedHandle;
	if(StartedHandle == Handle && Handle != Job.FinishedHandle.load())
	{
		// #if JQ_LOCKLESS_QUEUE
		return Job.PendingStart.load();
		// #else
		// 		uint16_t Start;
		// 		uint8_t	 Queue;
		// 		uint64_t StartAndQueue = Job.PendingStartAndQueue.load();
		// 		JqUnpackStartAndQueue(StartAndQueue, Start, Queue);
		// 		return Start;
		// #endif
	}
	return 0;
}

uint16_t JqTakeChildJobInternal(uint64_t Handle, uint16_t* OutSubIndex, uint64_t* HandleBuffer, uint32_t Size)
{
	if(!Size)
		return 0;
	uint16_t Index		 = JQ_GET_INDEX(Handle);
	JqJob&	 Job		 = JqState.Jobs[Index];
	uint32_t NumChildren = 0;
	if(Job.FirstChild == 0)
		return 0;

	{
		JqSingleMutexLock L(JqGetJobMutex(Index));
		uint16_t		  ChildIndex = Job.FirstChild;
		while(ChildIndex)
		{
			JqJob& ChildJob = JqState.Jobs[JQ_GET_INDEX(ChildIndex)];
			// only worry if there
			if(ChildJob.PendingStart.load() || (ChildJob.PendingFinish.load() & JOB_FINISH_CHILD_MASK) != 0)
			{
				uint64_t Claimed			= ChildJob.ClaimedHandle.load();
				HandleBuffer[NumChildren++] = Claimed;

				JQ_ASSERT(ChildJob.FinishedHandle.load() != Claimed); // finished will only be written once it's removed from the parents child list so this is safe to assert here.

				if(NumChildren == Size)
					break;
			}

			ChildIndex = ChildJob.NextSibling;
		}
	}
	JQ_ASSERT(NumChildren <= Size);

	// first try popping the children directly
	for(uint32_t i = 0; i < NumChildren; ++i)
	{
		uint64_t ChildHandle = HandleBuffer[i];
		if(JqTakeJobFromHandle(ChildHandle, OutSubIndex))
		{
			JQ_ASSERT(JqState.Jobs[JQ_GET_INDEX(ChildHandle)].StartedHandle.load() == JqState.Jobs[JQ_GET_INDEX(ChildHandle)].ClaimedHandle.load());
			return JQ_GET_INDEX(ChildHandle);
		}
	}
	// otherwise, recurse into their children
	for(uint32_t i = 0; i < NumChildren; ++i)
	{
		uint16_t Ret = JqTakeChildJobInternal(HandleBuffer[i], OutSubIndex, HandleBuffer + NumChildren, Size - NumChildren);
		if(Ret)
			return Ret;
	}
	return 0;
}

uint16_t JqTakeChildJob(uint64_t Handle, uint16_t* OutSubIndex)
{
	if(JqTakeJobFromHandle(Handle, OutSubIndex))
	{
		JQ_ASSERT(JqState.Jobs[JQ_GET_INDEX(Handle)].StartedHandle.load() == JqState.Jobs[JQ_GET_INDEX(Handle)].ClaimedHandle.load());
		return JQ_GET_INDEX(Handle);
	}
	JQ_MICROPROFILE_SCOPE("Inner take child job", MP_AUTO);
	uint64_t HandleBuffer[JQ_CHILD_HANDLE_BUFFER_SIZE];
	return JqTakeChildJobInternal(Handle, OutSubIndex, HandleBuffer, JQ_CHILD_HANDLE_BUFFER_SIZE);
}

bool JqExecuteOne()
{
	return JqExecuteOne(g_JqQueues, (uint8_t)g_nJqNumQueues);
}

bool JqExecuteOne(uint8_t nPipe)
{
	return JqExecuteOne(&nPipe, 1);
}

bool JqExecuteOne(uint8_t* nQueues, uint8_t nNumQueues)
{
	uint16_t nSubIndex = 0;
	uint16_t nWork	   = JqTakeJob(&nSubIndex, nNumQueues, nQueues);
	if(!nWork)
		return false;
	JqExecuteJob(JqState.Jobs[nWork].StartedHandle, nSubIndex);
	return true;
}

void JqWorker(int nThreadId)
{
	JqGetThreadState().WorkerState = JWS_WORKING;
	uint8_t* pQueues			   = JqState.QueueList[nThreadId];
	uint32_t nNumQueues			   = JqState.NumQueues[nThreadId];
	g_nJqNumQueues				   = nNumQueues; // even though its never usedm, its tagged because changing it is not supported.
	memcpy(g_JqQueues, pQueues, nNumQueues);
	int nSemaphoreIndex = JqState.SemaphoreIndex[nThreadId];

#if MICROPROFILE_ENABLED
	char sWorker[32];
	snprintf(sWorker, sizeof(sWorker) - 1, "JqWorker %d %08llx", nThreadId, JqState.SemaphoreMask[nSemaphoreIndex]);
	MicroProfileOnThreadCreate(&sWorker[0]);
#endif

	while(0 == JqState.Stop)
	{
		do
		{
			uint16_t nSubIndex = 0;
			uint16_t nWork	   = JqTakeJob(&nSubIndex, nNumQueues, pQueues);
			if(!nWork)
				break;
			JqExecuteJob(JqState.Jobs[nWork].StartedHandle, nSubIndex);
		} while(1);

		JqGetThreadState().WorkerState = JWS_IDLE;
		JqState.Semaphore[nSemaphoreIndex].Wait();
		JqGetThreadState().WorkerState = JWS_WORKING;
	}
	JqGetThreadState().WorkerState = JWS_NOT_WORKER;
#ifdef JQ_MICROPROFILE
	MicroProfileOnThreadExit();
#endif
}

void JqHandleJobQueueFull()
{
	if(JQ_QUEUE_FULL_EXECUTE_JOBS)
	{
		if(JqState.ActiveJobs >= (JQ_JOB_FILL_PRC_LIMIT * JQ_JOB_BUFFER_SIZE) / 100)
		{
			JqExecuteOne();
		}
	}
	else
	{
		JQ_BREAK(); // queue is close to being full. increase size, or deadlocks will occur.
	}
}

// Claim a handle. reset header, and initialize its pending count to 1
uint64_t JqClaimHandle()
{
	uint64_t h;
	// claim a handle atomically.
	JqJob* pJob = nullptr;
	while(1)
	{
		if(JqState.ActiveJobs >= (JQ_JOB_FILL_PRC_LIMIT * JQ_JOB_BUFFER_SIZE) / 100)
		{
			JqHandleJobQueueFull();
		}

		h = JqState.NextHandle.fetch_add(1);

		uint16_t index = h % JQ_JOB_BUFFER_SIZE;
		if(index == 0) // slot 0 is reserved
			continue;
		pJob = &JqState.Jobs[index];

		uint64_t finished = pJob->FinishedHandle.load();
		uint64_t claimed  = pJob->ClaimedHandle.load();
		uint64_t started  = pJob->StartedHandle.load();

		if(finished != claimed) // if the last claimed job is not yet done, we cant use it
			continue;
		if(started != finished)
		{
			// this shouldn't happen
			JQ_BREAK();
		}

		if(pJob->ClaimedHandle.compare_exchange_strong(claimed, h))
			break;
	}
	// reset header
	pJob->PendingFinish		= 0;
	pJob->PendingStart		= 0;
	pJob->ActiveQueue		= 0xff;
	pJob->PreconditionCount = 1;
	pJob->DependentJob.Job	= 0;
	pJob->DependentJob.Next = 0;
	pJob->NumJobs			= 0;
	pJob->NumJobsToStart	= 0;
	pJob->Range				= 0;
	pJob->JobFlags			= JQ_JOBFLAG_UNINITIALIZED;
	pJob->Waiters			= 0;
	pJob->Reserved			= false;

	pJob->Parent	  = 0;
	pJob->NextSibling = 0;
	pJob->PrevSibling = 0;

	pJob->FirstChild = 0;
	pJob->LastChild	 = 0;

	return h;
}

JQ_API void JqSpawn(JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t WaitFlag)
{
	JqHandle Handle = JqHandle{ 0 };
	if(0 == NumJobs)
	{
		return;
	}
	else if(1 == NumJobs)
	{
		// capture any jobs added as children
		Handle = JqGroupBegin();
		JqRunInternal(&JobFunc, 0, Range, (WaitFlag & JQ_JOBFLAG_EXTERNAL_MASK));
		JqGroupEnd();
	}
	else
	{
		Handle = JqAddInternal(JqHandle{ 0 }, JobFunc, Queue, NumJobs, Range, JQ_JOBFLAG_INTERNAL_SPAWN | (WaitFlag & JQ_JOBFLAG_EXTERNAL_MASK), JqHandle{ 0 });
		// Spawn tells the add that you want it to skip index 0, because its called immediately.
		// this is an atomic supported operation, so we always -force- spawn to claim the first entry
		JqExecuteJob(Handle.H, 0);
	}

	JqWait(Handle, WaitFlag);
}

void JqCancel(JqHandle Handle)
{

	if(JqIsDone(Handle))
	{
		return;
	}
	// Write to Job.Cancel the cancel value.
	// Executing jobs will check if this is set and skip execution

	const uint64_t H		= Handle.H;
	uint16_t	   JobIndex = JQ_GET_INDEX(H);
	JqJob&		   Job		= JqState.Jobs[JobIndex];

	uint64_t CancelHandle;
	do
	{
		CancelHandle = Job.Cancel.load();
		if(JQ_GT_WRAP(CancelHandle, H))
		{
			JQ_ASSERT(JqIsDone(Handle));
			return; // something later was cancelled
		}

	} while(!Job.Cancel.compare_exchange_weak(CancelHandle, H));
}

void JqIncPrecondtion(uint64_t Handle, int Count)
{
	uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
	JqJob*	 pJob  = &JqState.Jobs[Index];

	uint64_t Before = pJob->PreconditionCount.fetch_add(Count);
	// Adding preconditions is only allowed when the job has been reserved, in which case it'll have a count of 1 or larger
	// If we don't do it this way, jobs may be picked up before having their preconditions added.
	JQ_ASSERT(Before > 0);
}

void JqTriggerQueues(uint64_t QueueTriggerMask)
{
	JQ_MICROPROFILE_VERBOSE_SCOPE("TriggerQueues", MP_AUTO);
	bool SemaphoreTrigger[JQ_MAX_SEMAPHORES];
	for(bool& Trigger : SemaphoreTrigger)
		Trigger = false;
	uint8_t Queue = 0;
	while(QueueTriggerMask)
	{
		if(QueueTriggerMask & 1)
		{
			JQ_ASSERT(Queue < JQ_NUM_QUEUES);
			uint32_t nNumSema = JqState.QueueNumSemaphores[Queue];
			for(uint32_t i = 0; i < nNumSema; ++i)
			{
				int nSemaIndex				 = JqState.QueueToSemaphore[Queue][i];
				SemaphoreTrigger[nSemaIndex] = true;
			}
		}
		QueueTriggerMask <<= 1;
		Queue++;
	}
	uint32_t NumWorkers = JqState.NumWorkers;
	for(uint32_t i = 0; i < JQ_MAX_SEMAPHORES; ++i)
	{
		if(SemaphoreTrigger[i])
		{
			// Trigger all in the case of batch
			JqState.Semaphore[i].Signal(NumWorkers);
		}
	}
}

void JqDecPrecondtion(uint64_t Handle, int Count, uint64_t* QueueTriggerMask)
{
	uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job   = JqState.Jobs[Index];
	JQ_ASSERT(Job.ClaimedHandle == Handle);
	JQ_ASSERT(JQ_LT_WRAP(Job.StartedHandle, Handle));
	JQ_ASSERT(JQ_LT_WRAP(Job.FinishedHandle, Handle));

	uint64_t Before = Job.PreconditionCount.fetch_add(-Count);

	if(Before - Count == 0)
	{
		uint32_t NumJobs = Job.NumJobsToStart;
		JQ_ASSERT(Job.NumJobsToStart == Job.PendingStart.load());

		// this is the --only--  place StartedHandle is modified, except for -GroupBegin-

		Job.StartedHandle = Handle;
		if(NumJobs == 0) // Barrier type Jobs never have to enter an actual queue.
		{
			// JQ_ASSERT(
			JqFinishInternal(Index);
		}
		else
		{
			// Started handle is updated by JqQueuePush
			uint8_t Queue = Job.Queue;
			JqQueuePush(Queue, Handle);
			if(QueueTriggerMask)
			{
				JQ_ASSERT(Queue < 64);
				*QueueTriggerMask |= (1llu << Queue);
			}
			else
			{
				JQ_MICROPROFILE_VERBOSE_SCOPE("TriggerQueues2", MP_AUTO);
				uint32_t nNumSema = JqState.QueueNumSemaphores[Queue];
				for(uint32_t i = 0; i < nNumSema; ++i)
				{
					int nSemaIndex = JqState.QueueToSemaphore[Queue][i];
					JqState.Semaphore[nSemaIndex].Signal(NumJobs);
				}
			}
		}
	}
}

JqMutex& JqGetJobMutex(uint64_t Handle)
{
	return JqState.MutexJob[Handle % JQ_NUM_LOCKS];
}

JqConditionVariable& JqGetJobConditionVariable(uint64_t Handle)
{
	return JqState.ConditionVariableJob[Handle % JQ_NUM_LOCKS];
}

inline void JqUnpackQueueLink(uint64_t Value, uint16_t& Head, uint16_t& Tail, uint16_t& JobCount)
{
	Head	 = Value & 0xffff;
	Tail	 = (Value >> 16) & 0xffff;
	JobCount = (Value >> 32) & 0xffff;
}

inline uint64_t JqPackQueueLink(uint16_t Head, uint16_t Tail, uint16_t JobCount)
{
	uint64_t Value = Head | ((uint64_t)Tail << 16) | ((uint64_t)JobCount << 32);
	return Value;
}

inline void JqUnpackStartAndQueue(uint64_t Value, uint16_t& PendingStart, uint8_t& Queue)
{
	PendingStart = Value & 0xffff;
	Queue		 = (Value >> 16) & 0xff;
}

inline uint64_t JqPackStartAndQueue(uint16_t PendingStart, uint8_t Queue)
{
	uint64_t Value = PendingStart | (Queue << 16);
	return Value;
}

void JqQueuePush(uint8_t QueueIndex, uint64_t Handle)
{
	JQ_MICROPROFILE_SCOPE("JqQueuePush", MP_AUTO);
	uint16_t JobIndex = Handle % JQ_JOB_BUFFER_SIZE;

	JQ_ASSERT(QueueIndex < JQ_NUM_QUEUES);
	JqJob&			 Job   = JqState.Jobs[JobIndex];
	JqLocklessQueue& Queue = JqState.LocklessQueues[QueueIndex];

	JQ_ASSERT(Job.PreconditionCount == 0);
	JQ_ASSERT(Job.Queue == QueueIndex);

	uint64_t Started  = Job.StartedHandle;
	uint64_t Finished = Job.FinishedHandle;
	uint64_t Claimed  = Job.ClaimedHandle;
	JQ_ASSERT(Started == Claimed);

	Queue.Push(JobIndex);
	Job.ActiveQueue = QueueIndex;
}

// todo: this should verify the handle..
bool JqTryPopJob(uint16_t JobIndex, uint16_t* OutSubJob, bool& OutIsDrained)
{
	JQ_ASSERT(JobIndex < JQ_JOB_BUFFER_SIZE);
	JqJob& Job = JqState.Jobs[JobIndex];

	uint64_t Old, New;
	do
	{
		Old = Job.PendingStart.load();
		if(Old == 0)
		{
			OutIsDrained = true;
			return false;
		}
		New = Old - 1;
	} while(!Job.PendingStart.compare_exchange_weak(Old, New));
	OutIsDrained = New == 0;
	*OutSubJob	 = Job.NumJobs - 1 - (uint16_t)New;
	return true;
}
uint16_t JqQueuePop(uint8_t QueueIndex, uint16_t* OutSubIndex)
{
	JQ_ASSERT(QueueIndex < JQ_NUM_QUEUES);
	JqLocklessQueue& Queue = JqState.LocklessQueues[QueueIndex];
	JQ_MICROPROFILE_VERBOSE_SCOPE("Pop", MP_AUTO);

	do
	{
		uint32_t Value, Ref;
		if(!Queue.Peek(Value, &Ref))
		{
			return 0;
		}
		JQ_ASSERT(Value != 0);
		JQ_ASSERT(Value < JQ_JOB_BUFFER_SIZE);
		uint16_t SubIndex  = JQ_INVALID_SUBJOB;
		bool	 IsDrained = false;
		bool	 Success   = JqTryPopJob(Value, &SubIndex, IsDrained);
		if(!Success || IsDrained) // if we fail popping, or we're the last to be popped, try to remove
		{
			uint32_t OtherValue;
			if(Queue.Pop(OtherValue, &Ref)) // Failing is okay. what matters is someone will succeed with the pop'
			{
				JQ_ASSERT(OtherValue == Value);

				JqState.Jobs[Value].ActiveQueue = 0xff;
			}
		}
		if(Success)
		{
			*OutSubIndex = SubIndex;
			return Value;
		}
	} while(1);
}

uint16_t JqDependentJobLinkAlloc(uint64_t Owner)
{
	JqSingleMutexLock L(JqState.DependentJobLinkMutex);

	JqState.DependentJobLinkCounter++;

	uint16_t Link = JqState.DependentJobLinkHead;
	// printf("** DEP JOB ALLOCATE   %5d :: %d :: %5d\n", Link, JqState.DependentJobLinkCounter.load(), Owner);
	if(!Link)
	{
		// If you're hitting this, then you're adding more than JQ_JOB_BUFFER_SIZE, on top of the one link thats already space for
		// you'd probably want to make this allocator lockless and grow it significantly
		JQ_BREAK();
	}
	JqState.DependentJobLinkHead		  = JqState.DependentJobLinks[Link].Next;
	JqState.DependentJobLinks[Link].Next  = 0;
	JqState.DependentJobLinks[Link].Job	  = 0;
	JqState.DependentJobLinks[Link].Owner = 0;
	return Link;
}

void JqDependentJobLinkFreeList(uint16_t Link)
{
	JqSingleMutexLock L(JqState.DependentJobLinkMutex);
	uint16_t		  Head = JqState.DependentJobLinkHead;
	while(Link)
	{
		JQ_ASSERT(Link < JQ_JOB_BUFFER_SIZE);
		JqState.DependentJobLinkCounter--;
		// printf("** DEP JOB FREE       %5d :: %d :: %5d\n", Link, JqState.DependentJobLinkCounter.load(), JqState.DependentJobLinks[Link].Owner);

		uint16_t Next = JqState.DependentJobLinks[Link].Next;

		JqState.DependentJobLinks[Link].Next  = Head;
		JqState.DependentJobLinks[Link].Owner = 0;
		JqState.DependentJobLinks[Link].Job	  = 0;
		Head								  = Link;

		Link = Next;
	}
	JqState.DependentJobLinkHead = Head;
}

void JqAddPreconditionInternal(uint64_t Handle, uint64_t Precondition)
{
	// only add if its actually not done.
	if(!JqIsDone(JqHandle{ Precondition }))
	{
		uint16_t nIndex		   = Handle % JQ_JOB_BUFFER_SIZE;
		uint16_t nPrecondIndex = Precondition % JQ_JOB_BUFFER_SIZE;
		JqJob&	 Job		   = JqState.Jobs[nIndex];
		JqJob&	 PrecondJob	   = JqState.Jobs[nPrecondIndex];
		JQ_ASSERT(Job.PreconditionCount > 0); // as soon as the existing precond count reaches 0, it might be executed, so it no longer makes sense to add new preconditions
											  // use mechanisms like JqReserve, to block untill precondtions have been added.
		JqIncPrecondtion(Handle, 1);
		bool	 Finished;
		uint16_t LinkIndex = 0;
		while(true)
		{
			Finished = false;
			JQ_ASSERT(!LinkIndex); // only

			if(PrecondJob.DependentJob.Job)
			{
				LinkIndex = JqDependentJobLinkAlloc(Precondition);
			}

			JqSingleMutexLock L(JqGetJobMutex(Precondition));
			if(PrecondJob.FinishedHandle == Precondition)
			{
				Finished = true;
			}
			else
			{
				if(PrecondJob.DependentJob.Job == 0)
				{
					JQ_ASSERT(PrecondJob.DependentJob.Next == 0);
					PrecondJob.DependentJob.Job = Handle;
				}
				else
				{
					if(!LinkIndex && PrecondJob.DependentJob.Job) // Note: Can't (wont!) lock two mutexes, so in this exceptional condition we retry
						continue;
					JqDependentJobLink& L		 = JqState.DependentJobLinks[LinkIndex];
					L.Job						 = Handle;
					L.Next						 = PrecondJob.DependentJob.Next;
					PrecondJob.DependentJob.Next = LinkIndex;
					LinkIndex					 = 0; // clear to indicate it was successfully inserted
				}
			}
			break;
		}
		if(LinkIndex)
		{
			JqDependentJobLinkFreeList(LinkIndex);
		}
		if(Finished) // the precondition job finished after we took the lock, so decrement manually.
		{
			JqDecPrecondtion(Handle, 1);
		}
	}
}

void JqAddPrecondition(JqHandle Handle, JqHandle Precondition)
{
	if(!JqState.Jobs[Handle.H % JQ_JOB_BUFFER_SIZE].Reserved)
	{
		JQ_BREAK();
		// you can only add precondtions to jobs that have been Reserved and not yet added/closed
		// you must do
		// h = JqReserve
		//  -> Call JqAddPrecondition  here
		// JqCloseReserved(h)/JqAddReserved
	}
	JqAddPreconditionInternal(Handle.H, Precondition.H);
}

JqHandle JqAddInternal(JqHandle ReservedHandle, JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags, JqHandle PreconditionHandle)
{
	if(JobFlags & JQ_JOBFLAG_INTERNAL_SPAWN)
	{
		JQ_ASSERT(NumJobs > 1);
	}
	// Add:
	//	* Allocate header (inc counter)
	//	* Update header as reserved
	//	* Fill in header
	//  * if there is a parent, increment its pending count by no. of jobs
	//	* lock pipe mtx
	//  	* add to pipe
	// 	* unlock
	//

	uint64_t Parent = 0 != (JobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf().H;
	JQ_ASSERT(JqState.NumWorkers);
	JQ_ASSERT(NumJobs);
	JQ_ASSERT(NumJobs <= JQ_MAX_SUBJOBS);

	if(Range < 0)
	{
		Range = NumJobs;
	}
	if(NumJobs < 0)
	{
		NumJobs = JqState.NumWorkers;
	}
	if(NumJobs >= 0xffff)
	{
		// Not supported right now.
		JQ_BREAK();
	}

	uint64_t Handle = 0;
	{
		if(ReservedHandle.H)
		{
			JQ_ASSERT(Queue == 0xff);
			Handle = ReservedHandle.H;
		}
		else
		{
			Handle = JqClaimHandle();
		}

		uint16_t Index = Handle % JQ_JOB_BUFFER_SIZE;
		JqJob&	 Job   = JqState.Jobs[Index];

		if(ReservedHandle.H)
		{
			JQ_ASSERT(Job.ClaimedHandle == ReservedHandle.H);
			JQ_ASSERT(Job.Queue != 0xff);
			JQ_ASSERT(Job.PreconditionCount >= 1);
			if(!Job.Reserved)
			{
				JQ_BREAK(); // When reserving Job handles, you should call -either- JqCloseReserved or JqAddReserved, never both.
			}
			Job.Reserved = false;
		}
		else
		{
			JQ_ASSERT(Job.PreconditionCount == 1);
			JQ_ASSERT(Queue < JQ_NUM_QUEUES);
			Job.Queue = Queue;
			JQ_ASSERT(Job.Waiters == 0);
			// Job.Waiters = 0;
		}

		JQ_ASSERT(Job.ClaimedHandle.load() == Handle);
		JQ_ASSERT(JQ_LT_WRAP(Job.FinishedHandle, Handle));

		JQ_ASSERT(NumJobs <= 0xffff);

		Job.NumJobs		   = NumJobs;
		Job.NumJobsToStart = (0 != (JobFlags & JQ_JOBFLAG_INTERNAL_SPAWN)) ? NumJobs - 1 : NumJobs;
		Job.PendingStart   = Job.NumJobsToStart;
		JQ_ASSERT(Job.PendingFinish.load() == 0);
		Job.PendingFinish = NumJobs;

		Job.Range	 = Range;
		Job.JobFlags = JobFlags;

		JqAttachChild(Parent, Handle);
		JQ_ASSERT(Job.Parent < JQ_JOB_BUFFER_SIZE);
		JQ_ASSERT(Job.Parent == JQ_GET_INDEX(Parent));

		Job.Function = JobFunc;

		JqState.Stats.nNumAdded++;
		JqState.Stats.nNumAddedSub += NumJobs;
		if(PreconditionHandle.H)
		{
			JqAddPreconditionInternal(Handle, PreconditionHandle.H);
		}

		// Decrementing preconditions automatically add to a queue when reaching 0
		JQ_ASSERT(Job.PreconditionCount > 0);
		JqDecPrecondtion(Handle, 1);
	}
	return JqHandle{ Handle };
}

JqHandle JqAdd(JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(JqHandle{ 0 }, JobFunc, Queue, NumJobs, Range, JQ_JOBFLAG_EXTERNAL_MASK & JobFlags, JqHandle{ 0 });
}

// add reserved
JqHandle JqAddReserved(JqHandle ReservedHandle, JqFunction JobFunc, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(ReservedHandle, JobFunc, 0xff, NumJobs, Range, JQ_JOBFLAG_EXTERNAL_MASK & JobFlags, JqHandle{ 0 });
}

// add successor
JqHandle JqAddSuccessor(JqHandle PreconditionHandle, JqFunction JobFunc, uint8_t Queue, int NumJobs, int Range, uint32_t JobFlags)
{
	return JqAddInternal(JqHandle{ 0 }, JobFunc, Queue, NumJobs, Range, JQ_JOBFLAG_EXTERNAL_MASK & JobFlags, PreconditionHandle);
}

// Reserve a Job slot. this allows you to wait on work added later
JqHandle JqReserve(uint8_t Queue, uint32_t JobFlags)
{
	JQ_ASSERT(Queue < JQ_NUM_QUEUES);

	uint64_t Handle = JqClaimHandle();
	uint16_t Index	= Handle % JQ_JOB_BUFFER_SIZE;

	JqJob& Job = JqState.Jobs[Index];

	JQ_ASSERT(JQ_LE_WRAP(Job.FinishedHandle, Handle));
	JQ_ASSERT(JQ_LE_WRAP(Job.StartedHandle, Handle));
	JQ_ASSERT(Job.ClaimedHandle == Handle);
	// claim leaves precondition at 1 so we have to release it before we're ready.
	JQ_ASSERT(Job.PreconditionCount == 1);
	JQ_ASSERT(Job.NumJobs == 0);

	uint64_t Parent = 0 != (JobFlags & JQ_JOBFLAG_DETACHED) ? 0 : JqSelf().H;
	Job.Reserved	= 1;

	if(Parent)
	{
		JqAttachChild(Parent, Handle);
	}

	JQ_CLEAR_FUNCTION(Job.Function);
	Job.Queue	= Queue;
	Job.Waiters = 0;

	return JqHandle{ Handle };
}
// Mark reservation as finished, without actually executing any jobs.
void JqCloseReserved(JqHandle Handle)
{
	uint64_t H = Handle.H;
	if(!JqState.Jobs[H % JQ_JOB_BUFFER_SIZE].Reserved)
	{
		JQ_BREAK(); // When reserving Job handles, you should call -either- JqCloseReserved or JqAddReserved, never both.
	}
	JqState.Jobs[H % JQ_JOB_BUFFER_SIZE].Reserved = false;
	JqDecPrecondtion(H, 1);
}

bool JqIsDone(JqHandle Handle)
{
	uint64_t H				= Handle.H;
	uint64_t Index			= H % JQ_JOB_BUFFER_SIZE;
	uint64_t FinishedHandle = JqState.Jobs[Index].FinishedHandle;
	uint64_t Claimed		= JqState.Jobs[Index].ClaimedHandle;
	JQ_ASSERT(JQ_LE_WRAP(FinishedHandle, Claimed));
	int64_t nDiff = (int64_t)(FinishedHandle - H);
	JQ_ASSERT((nDiff >= 0) == JQ_LE_WRAP(H, FinishedHandle));
	return JQ_LE_WRAP(H, FinishedHandle);
}

bool JqIsDoneExt(JqHandle Handle, uint32_t nWaitFlags)
{
	uint64_t H		 = Handle.H;
	bool	 bIsDone = JqIsDone(Handle);
	if(!bIsDone && 0 != (nWaitFlags & JQ_WAITFLAG_IGNORE_CHILDREN))
	{
		uint64_t Index		   = H % JQ_JOB_BUFFER_SIZE;
		uint64_t PendingFinish = JqState.Jobs[Index].PendingFinish.load();
		// printf(" wait-ignore %llx :: %d .. %llx == %llx\n", PendingFinish, PendingFinish & JOB_FINISH_PLAIN_MASK, H, JqState.Jobs[Index].StartedHandle.load());
		return 0 == (PendingFinish & JOB_FINISH_PLAIN_MASK) && H == JqState.Jobs[Index].StartedHandle;
	}
	return bIsDone;
}

bool JqPendingJobs(uint64_t nJob)
{
	uint64_t nIndex = nJob % JQ_JOB_BUFFER_SIZE;
	JQ_ASSERT(JQ_LE_WRAP(JqState.Jobs[nIndex].FinishedHandle, JqState.Jobs[nIndex].StartedHandle));
	return JqState.Jobs[nIndex].FinishedHandle != JqState.Jobs[nIndex].ClaimedHandle;
}

void JqWaitAll()
{
	JQ_DEBUG_SCOPE(JDS_WAIT_ALL, 0, 0, 0);

	while(JqState.ActiveJobs > 0)
	{
		if(!JqExecuteOne())
		{
			JQ_USLEEP(1000);
		}
	}
}

void JqWait(JqHandle Handle, uint32_t WaitFlagArg, uint32_t UsWaitTime)
{
	const uint32_t WaitFlag = WaitFlagArg & JQ_JOBFLAG_EXTERNAL_MASK;
	uint64_t	   H		= Handle.H;

	// // uint64_t Start = 0;
	// if(WaitFlag & JQ_WAITFLAG_IGNORE_CHILDREN)
	// {
	// 	// Start = JqTick();
	// 	// printf("starting wait for children\n");
	// }

	if(JqIsDone(Handle))
	{
		return;
	}
	JQ_DEBUG_SCOPE(JDS_WAIT, Handle.H, WaitFlag, 0);

	while(!JqIsDoneExt(Handle, WaitFlag))
	{

		uint16_t SubIndex = JQ_INVALID_SUBJOB;
		uint16_t Work	  = 0;
		int		 mode	  = 0;
		if((WaitFlag & JQ_WAITFLAG_EXECUTE_CHILDREN) == JQ_WAITFLAG_EXECUTE_CHILDREN)
		{
			Work = JqTakeChildJob(H, &SubIndex);
			mode |= 1;
		}
		if(Work == 0 && JQ_WAITFLAG_EXECUTE_ANY == (WaitFlag & JQ_WAITFLAG_EXECUTE_ANY))
		{
			SubIndex = JQ_INVALID_SUBJOB;
			Work	 = JqTakeJob(&SubIndex, g_nJqNumQueues, g_JqQueues);
			mode |= 2;
		}

		if(Work)
		{
			JQ_ASSERT(SubIndex != JQ_INVALID_SUBJOB);

			JQ_ASSERT(JqState.Jobs[Work].StartedHandle.load());
			JQ_ASSERT(JqState.Jobs[Work].StartedHandle.load() == JqState.Jobs[Work].ClaimedHandle.load());
			JqExecuteJob(JqState.Jobs[Work].StartedHandle, SubIndex);
		}
		else
		{
			JQ_ASSERT(0 != (WaitFlag & (JQ_WAITFLAG_SLEEP | JQ_WAITFLAG_BLOCK | JQ_WAITFLAG_SPIN)));
			if(WaitFlag & JQ_WAITFLAG_SPIN)
			{
				uint64_t nTick			 = JqTick();
				uint64_t nTicksPerSecond = JqTicksPerSecond();
				do
				{
					uint32_t result = 0;
					for(uint32_t i = 0; i < 1000; ++i)
					{
						result |= i << (i & 7); // do something.. whatever
					}
					JqSpinloop |= result; // write it somewhere so the optimizer can't remote it
				} while((1000000ull * (JqTick() - nTick)) / nTicksPerSecond < UsWaitTime);
			}
			else if(WaitFlag & JQ_WAITFLAG_SLEEP)
			{
				JQ_USLEEP(UsWaitTime);
			}
			else
			{
				uint16_t		  nJobIndex = H % JQ_JOB_BUFFER_SIZE;
				JqMutex&		  Mutex		= JqGetJobMutex(nJobIndex);
				JqSingleMutexLock lock(Mutex);
				if(JqIsDoneExt(Handle, WaitFlag))
				{
					return;
				}
				JqState.Jobs[nJobIndex].Waiters++;
				JqState.Stats.nNumWaitCond++;
				JqGetJobConditionVariable(nJobIndex).Wait(Mutex);
				JqState.Jobs[nJobIndex].Waiters--;
			}
		}
	}
	// if(WaitFlag & JQ_WAITFLAG_IGNORE_CHILDREN)
	// {
	// 	// uint64_t End			= JqTick();
	// 	// uint64_t TicksPerSecond = JqTicksPerSecond();
	// 	// double	 MS				= 1000.0 * (End - Start) / TicksPerSecond;
	// 	// printf("Done Waiting for children  :: %fms\n", MS);
	// }
}

bool JqExecuteChild(JqHandle Handle)
{
	if(!JqIsDone(Handle))
	{
		uint16_t SubIndex = 0;
		uint16_t Work	  = JqTakeChildJob(Handle.H, &SubIndex);
		if(Work)
		{
			JqExecuteJob(JqState.Jobs[Work].StartedHandle, SubIndex);
			return true;
		}
	}
	return false;
}

void JqWaitAll(JqHandle* Jobs, uint32_t NumJobs, uint32_t WaitFlag, uint32_t UsWaitTime)
{
	for(uint32_t i = 0; i < NumJobs; ++i)
	{
		if(!JqIsDone(Jobs[i]))
		{
			JqWait(Jobs[i], WaitFlag, UsWaitTime);
		}
	}
}

uint64_t JqWaitAny(uint64_t* Jobs, uint32_t NumJobs, uint32_t nWaitFlag, uint32_t nUsWaitTime)
{
	JQ_BREAK(); // todo
	return 0;
}

JqHandle JqGroupBegin()
{
	uint64_t H	   = JqClaimHandle();
	uint16_t Index = H % JQ_JOB_BUFFER_SIZE;
	JqJob&	 Job   = JqState.Jobs[Index];

	JQ_ASSERT(JQ_LT_WRAP(Job.StartedHandle, H));
	JQ_ASSERT(JQ_LT_WRAP(Job.FinishedHandle, H));
	Job.NumJobs		   = 0;
	Job.NumJobsToStart = 0;
	Job.StartedHandle  = H;
	Job.PendingFinish  = 1;

	uint64_t Parent = JqSelf().H;
	if(Parent)
	{
		JqAttachChild(Parent, H);
	}
	JQ_ASSERT(Job.Parent == Parent);
	JQ_CLEAR_FUNCTION(Job.Function);
	Job.Queue = 0xff;
	JqSelfPush(H, 0);
	return JqHandle{ H };
}

void JqGroupEnd()
{
	uint64_t Job = JqSelf().H;
	JqSelfPop(Job);

	JqFinishSubJob(Job, 1);
}

JqHandle JqSelf()
{
	JqThreadState& State = JqGetThreadState();

	return JqHandle{ State.SelfPos ? State.SelfStack[State.SelfPos - 1].Job : 0 };
}

JqThreadState& JqGetThreadState()
{
	if(!ThreadState.Initialized)
	{
		JqMutexLock L(ThreadStateLock);
		JQ_ASSERT(!ThreadState.Initialized);
		{

			ThreadState.SelfPos			= 0;
			ThreadState.HasLock			= 0;
			ThreadState.WorkerState		= JWS_NOT_WORKER;
			ThreadState.DebugPos		= 0;
			ThreadState.Initialized		= 1;
			ThreadState.ThreadId		= JqGetCurrentThreadId();
			ThreadState.NextThreadState = FirstThreadState;
			ThreadState.SingleMutexPtr	= JqGetSingleMutexPtr();
			ThreadState.pJqNumQueues	= &g_nJqNumQueues;
			ThreadState.pJqQueues		= &g_JqQueues[0];

			FirstThreadState = &ThreadState;

			// todo: unregister on thread exit..
		}
	}
	return ThreadState;
}
// split handle for easier debugging
void JqSplitHandle(uint64_t Handle, uint64_t& Index, uint64_t& Generation)
{
	Index	   = Handle % JQ_JOB_BUFFER_SIZE;
	Generation = Handle >> JQ_JOB_BUFFER_SHIFT;
}
void JqDump()
{
}
void JqDumpState()
{
	printf("\n\nThreads:\n");
	{
		JqMutexLock	   L(ThreadStateLock);
		JqThreadState* State = FirstThreadState;
		while(State)
		{
			printf("%16p: %p %10s %3d q:[", (void*)State->ThreadId, State->SingleMutexPtr, JqWorkerStateString(State->WorkerState), State->DebugPos);
			for(uint32_t i = 0; i < *State->pJqNumQueues; ++i)
				printf("%2d", State->pJqQueues[i]);
			printf("]\n");

			if(1)
			{
				for(int i = 0; i < State->DebugPos; ++i)
				{
					JqDebugState& S = State->DebugStack[State->DebugPos - 1 - i];
					uint64_t	  Index, Generation;
					JqSplitHandle(S.Handle, Index, Generation);
					printf("%60s %10s %4x %6d, %8llx(%04llx/%08llx)\n", "", JqDebugStackStateString(S.State), S.Flags, S.SubIndex, S.Handle, Index, Generation);
				}
			}

			State = State->NextThreadState;
		}
	}
	printf("\nQueues:\n");
	for(uint32_t i = 0; i < JQ_NUM_QUEUES; ++i)
	{
		JqLocklessQueue& Queue = JqState.LocklessQueues[i];
		printf("Queue %d: \n", i);

		// /Function(Pop, PopIndex, EntryPushSequence, EntryPopSequence, Payload);

		Queue.DebugCallbackAll([&](int Index, int WrappedIndex, uint16_t Seq, uint16_t PushSequence, uint16_t PopSequence, uint32_t Payload) {
			uint64_t Handle = JqState.Jobs[Payload % JQ_JOB_BUFFER_SIZE].StartedHandle.load();
			uint64_t HandleIndex, HandleGeneration;
			JqSplitHandle(Handle, HandleIndex, HandleGeneration);
			printf("\tElement     %d/%5d [%4x==%4x==%4x] :: %d  Handle %8llx(%04llx/%08llx)\n", WrappedIndex, Index, Seq, PushSequence, PopSequence, Payload, Handle, HandleIndex, HandleGeneration);
		});
	}

	printf("\nUnfinished Jobs\n");

	printf("                                                                PreconditionCount\n");
	printf("                                          PendingFinish             Parent \n");
	printf("Reserved                                          PendingStart                     Dependent              Num        Waiters     Prev\n");
	printf("  Base / Claimed  / Started  / Finished |               Queue|                                   DepLink|       Queue     Next    \n");
	printf("-------------------------------------------------------------------------------------------------------------------------\n");

	for(uint32_t i = 0; i < JQ_JOB_BUFFER_SIZE; ++i)
	{
		JqJob& Job = JqState.Jobs[i];
		if(Job.StartedHandle != Job.FinishedHandle || Job.StartedHandle != Job.ClaimedHandle)
		{
			uint8_t	 Queue;
			uint16_t PendingStart;
			// #if JQ_LOCKLESS_QUEUE
			PendingStart = Job.PendingStart.load();
			Queue		 = Job.ActiveQueue.load();
			// #else
			// 			JqUnpackStartAndQueue(Job.PendingStartAndQueue.load(), PendingStart, Queue);
			// #endif
			uint64_t ClaimedGeneration, ClaimedIndex;
			uint64_t StartedGeneration, StartedIndex;
			uint64_t FinishedGeneration, FinishedIndex;
			uint64_t ParentGeneration, ParentIndex;
			uint64_t DependentGeneration, DependentIndex;
			JqSplitHandle(Job.ClaimedHandle.load(), ClaimedIndex, ClaimedGeneration);
			JqSplitHandle(Job.StartedHandle.load(), StartedIndex, StartedGeneration);
			JqSplitHandle(Job.FinishedHandle.load(), FinishedIndex, FinishedGeneration);
			JqSplitHandle(Job.DependentJob.Job, DependentIndex, DependentGeneration);
			uint64_t ParentHandle = JqState.Jobs[Job.Parent].ClaimedHandle.load();
			JqSplitHandle(ParentHandle, ParentIndex, ParentGeneration);
			if(ClaimedIndex != i && ClaimedIndex != 0)
			{
				printf("fail Claimed %lld %d", ClaimedIndex, i);
				JQ_BREAK();
			}
			if(StartedIndex != i && StartedIndex != 0)
			{
				printf("fail Started %lld %d", StartedIndex, i);
				JQ_BREAK();
			}
			if(FinishedIndex != i && FinishedIndex != 0)
			{
				printf("fail Finished %lld %d", FinishedIndex, i);
				JQ_BREAK();
			}
			printf("%c %04x / %08llx / %08llx / %08llx | %05d / %05d / %2x | %3lld [%04llx/%08llx] [%04llx/%08llx] %04x | %3d / %02x / %02x / %04x / %04x\n", Job.Reserved ? 'R' : ' ', i,
				   ClaimedGeneration, StartedGeneration, FinishedGeneration, (uint16_t)Job.PendingFinish.load(), PendingStart, Queue, Job.PreconditionCount.load(), ParentIndex, ParentGeneration,
				   DependentIndex, DependentGeneration, Job.DependentJob.Next, Job.NumJobs, Job.Queue, Job.Waiters, Job.NextSibling, Job.PrevSibling);
		}
	}
}

const char* JqWorkerStateString(JqWorkerState State)
{
	switch(State)
	{
	case JWS_NOT_WORKER:
		return "NOT_WORKER";
	case JWS_WORKING:
		return "WORKING";
	case JWS_IDLE:
		return "IDLE";
	}
	JQ_BREAK();
	return "";
}
const char* JqDebugStackStateString(JqDebugStackState State)
{
	switch(State)
	{
	case JDS_EXECUTE:
		return "EXECUTE";
	case JDS_WAIT:
		return "WAIT";
	case JDS_WAIT_ALL:
		return "WAIT_ALL";
	case JDS_INVALID:
		return "INVALID";
	}
	JQ_BREAK();
	return "";
}
