#pragma once
//based on boost.context


typedef void*   JqFContext;
struct JqTransfer {
    JqFContext  fctx;
    void    *   data;
};

extern "C"
JqTransfer  jump_fcontext( JqFContext const to, void * vp);
extern "C" 
JqFContext  make_fcontext( void * sp, std::size_t size, void (* fn)( JqTransfer) );
