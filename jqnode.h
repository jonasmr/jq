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
	JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs, int nRange);
	~JqNode();
	void Run();
	void After(JqNode& pNode);
	void Wait();
private:
	void RunInternal();
	JqFunction JobFunc;
	uint64_t nMasterJob;
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
//uint64_t JqAdd(JqFunction JobFunc, uint8_t nPipe, void* pArg, int nNumJobs, int nRange, uint64_t nParent)


#ifdef JQ_IMPL

JqNode::JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs, int nRange)
	:JobFunc(Func)
	,nJob(0)
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
	JQ_ASSERT(m_State == STATE_DONE);
}

void JqNode::Run()
{
	JQ_ASSERT(nMasterJob == 0);
	JQ_ASSERT(m_State == STATE_INIT);
	nMasterJob = JqAdd(
		[this](int b, int e){
			RunInternal();
		}, nPipe, 1, 1);
}
void JqNode::After(JqNode& Node)
{
	JQ_ASSERT(nMasterJob == 0);
	JQ_ASSERT(Node.nMasterJob == 0); // must be called before job is started
	JQ_ASSERT(Node.NumDependent < JQ_NODE_MAX_DEPENDENT_JOBS);
	JQ_ASSERT(Node.m_State == STATE_INIT);
	JQ_ASSERT(m_State == STATE_INIT);
	Node.Dependent[Node.NumDependent++] = this;
	pParent = &Node;
}
void JqNode::Wait()
{
	if(pParent)
		pParent->Wait();
	else
	{
		JQ_ASSERT(nMasterJob);
		JqWait(nMasterJob);
	}
}
void JqNode::RunInternal()
{
	uint64_t nJob = JqAdd(JobFunc, nPipe, nNumJobs, nRange);
	if(NumDependent)
	{
		JqWait(nJob);
		for(uint32_t i = 0; i < NumDependent; ++i)
		{
			JqNode* pNode = Dependent[i]; 
			JqAdd([pNode](int b, int e)
			{
				pNode->RunInternal();
			},
		}
		JqAdd([]
	}
}
#endif

