#include "objfiles/objfiles.h"
#include "common/ts_std_common.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static TS_Status of_exists(TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e) {
  const char *p; struct stat st; (void)u;
  if (ts_std_argc(vm,n,1,1,e,"exists") || ts_std_str(&a[0],&p,e,"exists")) return TS_ERR_INVAL;
  ts_std_ret_bool(r, stat(p,&st)==0); return TS_OK;
}
static TS_Status of_size(TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e) {
  const char *p; struct stat st; (void)u;
  if (ts_std_argc(vm,n,1,1,e,"size") || ts_std_str(&a[0],&p,e,"size")) return TS_ERR_INVAL;
  if (stat(p,&st)!=0) { ts_error_set(e,TS_ERR_SYSTEM,errno,0,"size: cannot stat file"); return TS_ERR_SYSTEM; }
  if (st.st_size<0) return ts_std_inval(e,"size: invalid file size"); ts_std_ret_int(r,(int64_t)st.st_size); return TS_OK;
}
static TS_Status of_read(TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e) {
  const char *p; FILE *f; ts_sbuf_t b; unsigned char buf[8192]; size_t got; char *out; (void)u;
  if (ts_std_argc(vm,n,1,1,e,"read") || ts_std_str(&a[0],&p,e,"read")) return TS_ERR_INVAL;
  f=fopen(p,"rb"); if(!f){ts_error_set(e,TS_ERR_SYSTEM,errno,0,"read: cannot open file"); return TS_ERR_SYSTEM;}
  ts_sbuf_init(&b); while((got=fread(buf,1,sizeof buf,f))>0) if(ts_sbuf_put(&b,buf,got)!=0){fclose(f);ts_sbuf_free(&b);return ts_std_oom(e);} if(ferror(f)){int er=errno;fclose(f);ts_sbuf_free(&b);ts_error_set(e,TS_ERR_SYSTEM,er,0,"read: I/O error");return TS_ERR_SYSTEM;} fclose(f);
  out=ts_sbuf_take(&b); ts_value_free(r); r->type=TS_STRING; r->as.string=out; if(!out)return ts_std_oom(e); return TS_OK;
}
static TS_Status of_write(TS_VM *vm, void *u, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e) {
  const char *p,*data; FILE *f; size_t len,w; (void)u;
  if(ts_std_argc(vm,n,2,2,e,"write") || ts_std_str(&a[0],&p,e,"write") || ts_std_str(&a[1],&data,e,"write")) return TS_ERR_INVAL;
  f=fopen(p,"wb"); if(!f){ts_error_set(e,TS_ERR_SYSTEM,errno,0,"write: cannot open file");return TS_ERR_SYSTEM;} len=strlen(data); w=fwrite(data,1,len,f); if(w!=len || fclose(f)!=0){ts_error_set(e,TS_ERR_SYSTEM,errno,0,"write: I/O error");return TS_ERR_SYSTEM;} ts_std_ret_int(r,(int64_t)w); return TS_OK;
}
static const TS_FuncDef funcs[]={{"exists",of_exists,NULL},{"size",of_size,NULL},{"read",of_read,NULL},{"write",of_write,NULL}};
const TS_Module ts_std_objfiles_module=TS_MODULE_INIT("std.objfiles",funcs);
