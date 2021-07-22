#pragma once
#include <stddef.h>

// based on boost.context

typedef void* JqFContext;
struct JqTransfer
{
	JqFContext fctx;
	void*	   data;
};

extern "C" JqTransfer jq_jump_fcontext(JqFContext const to, void* vp);
extern "C" JqFContext jq_make_fcontext(void* sp, size_t size, void (*fn)(JqTransfer));
