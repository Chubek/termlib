#include "ffi/ffi.h"
#include "common/ts_std_common.h"
#include <dynalo/dynalo.hpp>
#include <memory>
#include <stdexcept>
#include <string>

struct ffi_lib { std::shared_ptr<dynalo::library> lib; };
struct ffi_fn { std::shared_ptr<dynalo::library> lib; void *ptr; };
static const char lib_tag = 0, fn_tag = 0;
static void free_lib(void *p) { delete (ffi_lib*)p; }
static void free_fn(void *p) { delete (ffi_fn*)p; }
static ffi_lib *get_lib(const TS_Value *v, TS_Error *e, const char *w) { return (ffi_lib*)ts_std_handle(v,&lib_tag,e,w); }
static ffi_fn *get_fn(const TS_Value *v, TS_Error *e, const char *w) { return (ffi_fn*)ts_std_handle(v,&fn_tag,e,w); }
static TS_Status arity(TS_VM *v,size_t n,size_t lo,size_t hi,TS_Error *e,const char *w){return ts_std_argc(v,n,lo,hi,e,w);}
static TS_Status f_load(TS_VM *v,void*,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){
  const char *p; if(arity(v,n,1,1,e,"load")||ts_std_str(a,&p,e,"load")) return TS_ERR_INVAL;
  try { auto *x=new ffi_lib; x->lib=std::make_shared<dynalo::library>(std::string(p)); if(ts_value_make_handle(r,x,free_lib,"ffi library",&lib_tag)!=TS_OK){delete x; return ts_std_oom(e);} return TS_OK; }
  catch(const std::exception &ex){ts_error_set(e,TS_ERR_SYSTEM,0,0,"load: %s",ex.what()); return TS_ERR_SYSTEM;}
}
static TS_Status f_symbol(TS_VM *v,void*,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){
  const char *name; ffi_lib *l; if(arity(v,n,2,2,e,"symbol")||!(l=get_lib(a,e,"symbol"))||ts_std_str(a+1,&name,e,"symbol")) return TS_ERR_INVAL;
  try { auto *x=new ffi_fn; x->lib=l->lib; x->ptr=(void*)x->lib->get_function<void()>(name); if(ts_value_make_handle(r,x,free_fn,"ffi function",&fn_tag)!=TS_OK){delete x; return ts_std_oom(e);} return TS_OK; }
  catch(const std::exception &ex){ts_error_set(e,TS_ERR_INVAL,0,0,"symbol: %s",ex.what()); return TS_ERR_INVAL;}
}
static bool get_i(const TS_Value& v,int64_t& x){if(v.type!=TS_INT)return false;x=v.as.integer;return true;}
static TS_Status f_call(TS_VM *v,void*,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){
  ffi_fn *f=get_fn(a,e,"call"); const char *sig; if(!f||n<2||ts_std_str(a+1,&sig,e,"call")) return TS_ERR_INVAL;
  size_t argc=0; while(sig[argc+1] && sig[argc+1]!=')') argc++; if(sig[0]!='i'||sig[argc+1]!=')'||n!=argc+2){ts_error_set(e,TS_ERR_INVAL,0,0,"call: signature must be i(...), with matching arguments");return TS_ERR_INVAL;}
  if(argc==0){int (*fn)()=(int(*)())f->ptr; ts_std_ret_int(r,fn()); return TS_OK;}
  if(argc==1){int64_t x;if(!get_i(a[2],x)){ts_error_set(e,TS_ERR_INVAL,0,0,"call: integer argument required");return TS_ERR_INVAL;} int (*fn)(int)=(int(*)(int))f->ptr;ts_std_ret_int(r,fn((int)x));return TS_OK;}
  if(argc==2){int64_t x,y;if(!get_i(a[2],x)||!get_i(a[3],y)){ts_error_set(e,TS_ERR_INVAL,0,0,"call: integer arguments required");return TS_ERR_INVAL;} int (*fn)(int,int)=(int(*)(int,int))f->ptr;ts_std_ret_int(r,fn((int)x,(int)y));return TS_OK;}
  ts_error_set(e,TS_ERR_LIMIT,0,0,"call: at most two arguments supported"); return TS_ERR_LIMIT;
}
static const TS_FuncDef funcs[]={{"load",f_load,NULL},{"symbol",f_symbol,NULL},{"call",f_call,NULL}};
const TS_Module ts_std_ffi_module=TS_MODULE_INIT("std.ffi",funcs);
