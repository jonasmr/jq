#include "jqnode.h"
#include "string.h"

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
	if(nNumJobsExecuted.fetch_add(1)+1 == (uint32_t)nNumJobs)
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