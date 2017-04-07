#pragma once
#include "jq.h"
#include "jqinternal.h"
#include <atomic>

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

struct JqNode;

JQ_API JqNode* JqNodeSelf();


struct JqNode
{
	JqNode(JqFunction Func, uint8_t nPipe, int nNumJobs = 1, int nRange = -1);
	virtual ~JqNode();
	void Run();
	void After(JqNode& Node);
	void After(JqNode& NodeA,JqNode& NodeB);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE);
	void After(JqNode& NodeA,JqNode& NodeB, JqNode& NodeC, JqNode& NodeD, JqNode& NodeE, JqNode& NodeF);
	void Wait();
	virtual void Reset();
private:
	JqNode(const JqNode&);
	JqNode& operator =(const JqNode&);
	void RunInternal(int b, int e);
	void KickInternal();
	void DependencyDone();
	virtual void SetResultVoid(void* pResult){};
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



template<typename T>
struct JqNodeResult : public JqNode
{
	T Result;
	bool bResultSet;
	JqNodeResult(JqFunction Func, uint8_t nPipe);
	T AwaitResult()
	{
		Wait();
		JQ_ASSERT(bResultSet);
		return Result;
	}
	void SetResult(T* t){ bResultSet = true; Result = *t; }
	void SetResultVoid(void* pResult){ SetResult( (T*)pResult ); }
	void Reset(){
		JqNode::Reset();
		bResultSet = false;
	}
};

template<typename T>
JqNodeResult<T>::JqNodeResult(JqFunction Func, uint8_t nPipe)
:JqNode(Func, nPipe)
,bResultSet(false)
{
}

template<typename T>
void JqNodeSetResult(T& t)
{
	JqNode* pNode = JqNodeSelf();

	((JqNodeResult<T>*)pNode)->SetResultVoid((void*)&t);
}



