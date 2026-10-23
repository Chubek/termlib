#include "termart/termarch.h"
#include "common/ts_std_common.h"
#include <string.h>
static TS_Status ta_name(TS_VM *v,void *u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){(void)u;(void)a;if(ts_std_argc(v,n,0,0,e,"name")!=TS_OK)return TS_ERR_INVAL;return ts_std_ret_str(r,"ansi",e);}
static TS_Status ta_escape(TS_VM*v,void*u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){const char*s;(void)u;if(ts_std_argc(v,n,1,1,e,"escape")!=TS_OK||ts_std_str(&a[0],&s,e,"escape")!=TS_OK)return TS_ERR_INVAL;return ts_std_ret_str(r,s,e);}
static const TS_FuncDef f[]={{"name",ta_name,NULL},{"escape",ta_escape,NULL}};
const TS_Module ts_std_termarch_module=TS_MODULE_INIT("std.termarch",f);
