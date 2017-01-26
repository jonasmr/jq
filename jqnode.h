#pragma once
#include "jq.h"

//helper class for making preconfigured "node" based jobs
//
// IE
// JqNode A(..);
// JqNode B(..);
// JqNode C(..);
// JqNode D(..);
// B.After(A);
// C.After(B);
// D.After(B, C);
// A.Run();
//
//
// A.Wait();

#ifndef JQ_NODE_MAX_DEPENDENT_JOBS
#define JQ_NODE_MAX_DEPENDENT_JOBS 8
#endif

#define JQNODE_VERIFY 1

#if JQNODE_VERIFY
#define JQNODE_STATE_VERIFY(exp) JQ_ASSERT(exp);
#define JQNODE_STATE_NEXT(s) do{State = s;}while(0)
#else
#define JQNODE_STATE_VERIFY(cur) do{}while(0)
#define JQNODE_STATE_NEXT(s) do{}while(0)
#endif

struct JqNode
{
	JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs = 1, int nRange = -1);
	~JqNode();
	void Run();
	void After(JqNode& Node);
	void After(JqNode& NodeA,JqNode& NodeB);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE, JqNode& NodeF);
	void Wait();
	void Reset();
private:
	JqNode(const JqNode&);
	JqNode& operator =(const JqNode&);
	void RunInternal(int b, int e);
	void KickInternal();
	void DependencyDone();
	JqFunction JobFunc;
	uint64_t nJob;
	const int nNumJobs; // must be set on construction and never change.
	int nRange;
	uint8_t nPipe;
	enum
	{
		STATE_INIT,
		STATE_KICKED,
		STATE_RUNNING,
		STATE_DONE,
	};
	uint8_t State;
	std::atomic<uint32_t> NumJobDependent;
	std::atomic<uint32_t> NumJobFinished;
#if JQNODE_VERIFY
	std::atomic<uint32_t> nNumJobsExecuted;
#endif

	uint32_t NumDependencies;
	JqNode* Dependent[JQ_NODE_MAX_DEPENDENT_JOBS];
};

#ifdef JQ_IMPL

JqNode::JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs, int nRange)
	:JobFunc(Func)
	,nJob(0)
	,nNumJobs(nNumJobs)
	,nRange(nRange)
	,nPipe(nPipe)
	,State(STATE_INIT)
	,NumJobDependent(0)
	,NumJobFinished(0)
#if JQNODE_VERIFY
	,nNumJobsExecuted(0)
#endif
	,NumDependencies(0)
{
	memset(&Dependent[0], 0, sizeof(Dependent));
}
JqNode::~JqNode()
{
	JQNODE_STATE_VERIFY(STATE_DONE == State);
	JQ_ASSERT(JqIsDone(nJob));
}

void JqNode::Run()
{
	JQNODE_STATE_VERIFY(STATE_INIT == State);
	KickInternal();
}

void JqNode::KickInternal()
{
	JQ_ASSERT(nJob == 0);
	JQNODE_STATE_VERIFY(State == STATE_INIT);
	JQ_ASSERT(NumJobFinished.load() == NumJobDependent.load());
	JQNODE_STATE_NEXT(STATE_KICKED);
	nJob = JqAdd(
		[this](int b, int e){
			RunInternal(b, e);
		}, nPipe, nNumJobs, nRange);
}
void JqNode::After(JqNode& Node)
{
	JQNODE_STATE_VERIFY(State == STATE_INIT);
	JQNODE_STATE_VERIFY(Node.State == STATE_INIT);
	JQ_ASSERT(nJob == 0);
	JQ_ASSERT(Node.nJob == 0); // must be called before job is started
	JQ_ASSERT(Node.NumDependencies < JQ_NODE_MAX_DEPENDENT_JOBS);
	//assert not double adding
	for(uint32_t i = 0; i < Node.NumDependencies; ++i)
	{
		JQ_ASSERT(Node.Dependent[i] != this);
	}
	Node.Dependent[Node.NumDependencies++] = this;
	NumJobDependent += Node.nNumJobs;
}
void JqNode::After(JqNode& NodeA, JqNode& NodeB)
{
	After(NodeA);
	After(NodeB);
}
void JqNode::After(JqNode& NodeA, JqNode& NodeB, JqNode& NodeC)
{
	After(NodeA);
	After(NodeB);
	After(NodeC);

}
void JqNode::After(JqNode& NodeA, JqNode& NodeB, JqNode& NodeC, JqNode& NodeD)
{
	After(NodeA);
	After(NodeB);
	After(NodeC);
	After(NodeD);
}
void JqNode::After(JqNode& NodeA, JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE)
{
	After(NodeA);
	After(NodeB);
	After(NodeC);
	After(NodeD);
	After(NodeE);
}
void JqNode::After(JqNode& NodeA, JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE, JqNode& NodeF)
{
	After(NodeA);
	After(NodeB);
	After(NodeC);
	After(NodeD);
	After(NodeE);
	After(NodeF);
}


void JqNode::Reset()
{
	JQNODE_STATE_VERIFY(State == STATE_INIT || State == STATE_DONE);
	if(nJob)
	{
		JqWait(nJob);
		nJob = 0;
	}
	JQNODE_STATE_NEXT(STATE_INIT);
	NumJobFinished.store(0);
#if JQNODE_VERIFY
	nNumJobsExecuted.store(0);
#endif
	for(uint32_t i = 0; i < NumDependencies; ++i)
	{
		Dependent[i]->Reset();
	}
}


void JqNode::Wait()
{
	JQNODE_STATE_VERIFY(State != STATE_INIT);
	JQ_ASSERT(NumJobDependent.load() == 0); //only callable on root jobs
	JQ_ASSERT(nJob != 0);
	{
		JQ_ASSERT(nJob);
		JqWait(nJob);
	}
	JQNODE_STATE_VERIFY(State == STATE_DONE);
	nJob = 0;
}
void JqNode::DependencyDone()
{
	uint32_t nValue = NumJobFinished.fetch_add(1);
	if(nValue+1 == NumJobDependent.load())
	{
		KickInternal();
	}
}
void JqNode::RunInternal(int b, int e)
{
	JobFunc(b, e);
#if JQNODE_VERIFY
	if(nNumJobsExecuted.fetch_add(1)+1 == nNumJobs)
	{
		JQNODE_STATE_VERIFY(State == STATE_KICKED);
		JQNODE_STATE_NEXT(STATE_DONE);
	}
#endif
	for(uint32_t i = 0; i < NumDependencies; ++i)
	{
		Dependent[i]->DependencyDone();
	}
}
#endif

