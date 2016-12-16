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
	void ResetGraph();
private:
	void RunInternal(int b, int e);
	void KickInternal();
	void DependencyDone();
	JqFunction JobFunc;
	uint64_t nJob;
	int nNumJobs;
	int nRange;
	uint8_t nPipe;
	enum
	{
		STATE_INIT,
		STATE_RUNNING,
		STATE_DEPENDENT,
		STATE_DONE,
	};
	uint8_t State;
	std::atomic<uint32_t> NumJobDependent;
	std::atomic<uint32_t> NumJobFinished;
	uint32_t NumDependent;
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
	,NumDependent(0)
{
	memset(&Dependent[0], 0, sizeof(Dependent));
}
JqNode::~JqNode()
{
	JQ_ASSERT(JqIsDone(nJob));
}

void JqNode::Run()
{
	KickInternal();
}

void JqNode::KickInternal()
{
	JQ_ASSERT(nJob == 0);
	JQ_ASSERT(State == STATE_INIT);
	JQ_ASSERT(NumJobFinished.load() == NumJobDependent.load());
	nJob = JqAdd(
		[this](int b, int e){
			RunInternal(b, e);
		}, nPipe, nNumJobs, nRange);
}
void JqNode::After(JqNode& Node)
{
	JQ_ASSERT(nJob == 0);
	JQ_ASSERT(Node.nJob == 0); // must be called before job is started
	JQ_ASSERT(Node.NumDependent < JQ_NODE_MAX_DEPENDENT_JOBS);
	//assert not double adding
	for(uint32_t i = 0; i < Node.NumDependent; ++i)
	{
		JQ_ASSERT(Node.Dependent[i] != this);
	}
	Node.Dependent[Node.NumDependent++] = this;
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


void JqNode::ResetGraph()
{
	JQ_ASSERT(State == STATE_INIT);
	NumJobFinished.store(0);
	if(nJob)
	{
		JqWait(nJob);
		nJob = 0;
	}
	for(uint32_t i = 0; i < NumDependent; ++i)
	{
		Dependent[i]->ResetGraph();
	}


}


void JqNode::Wait()
{
	JQ_ASSERT(NumJobDependent.load() == 0); //only callable on root jobs
	JQ_ASSERT(nJob != 0);
	{
		JQ_ASSERT(nJob);
		JqWait(nJob);
	}
	nJob = 0;
	State = STATE_INIT;
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
	for(uint32_t i = 0; i < NumDependent; ++i)
	{
		Dependent[i]->DependencyDone();
	}
}
#endif

