#pragma once
#include "jq.h"

//helper class for making preconfigured "node" based jobs
//
// IE
// JqNode A(..);
// JqNode B(..);
// JqNode C(..);
// B.After(A);
// C.After(B);
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
	void After(JqNode& pNode);
	void Wait();
private:
	void RunInternal();
	void KickInternal();
	JqFunction JobFunc;
	uint64_t nControlJob;
	uint64_t nWorkJob;
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
	uint8_t NumDependent;
	JqNode* pParent;
	JqNode* Dependent[JQ_NODE_MAX_DEPENDENT_JOBS];
};

#ifdef JQ_IMPL

JqNode::JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs, int nRange)
	:JobFunc(Func)
	,nControlJob(0)
	,nWorkJob(0)
	,nNumJobs(nNumJobs)
	,nRange(nRange)
	,nPipe(nPipe)
	,State(STATE_INIT)
	,NumDependent(0)
	,pParent(nullptr)
{
	memset(&Dependent[0], 0, sizeof(Dependent));
}
JqNode::~JqNode()
{
	JQ_ASSERT(JqIsDone(nControlJob));
}

void JqNode::Run()
{
	KickInternal();
}

void JqNode::KickInternal()
{
	JQ_ASSERT(nControlJob == 0);
	JQ_ASSERT(State == STATE_INIT);
	nControlJob = JqAdd(
		[this](int b, int e){
			RunInternal();
		}, nPipe, 1, 1);
}
void JqNode::After(JqNode& Node)
{
	JQ_ASSERT(nControlJob == 0);
	JQ_ASSERT(Node.nControlJob == 0); // must be called before job is started
	JQ_ASSERT(Node.NumDependent < JQ_NODE_MAX_DEPENDENT_JOBS);
	JQ_ASSERT(nControlJob == 0);
	Node.Dependent[Node.NumDependent++] = this;
	pParent = &Node;
}
void JqNode::Wait()
{
	if(pParent)
		pParent->Wait();
	else
	{
		JQ_ASSERT(nControlJob);
		JqWait(nControlJob);
	}
	nWorkJob = 0;
	nControlJob = 0;
}
void JqNode::RunInternal()
{
	JQ_ASSERT(nWorkJob == 0);
	nWorkJob = JqAdd(JobFunc, nPipe, nNumJobs, nRange);
	if(NumDependent)
	{
		JqWait(nWorkJob);
		for(uint32_t i = 0; i < NumDependent; ++i)
		{
			Dependent[i]->KickInternal();
		}
	}
}
#endif

