#include "termbox/termscriot.h"
#include "common/ts_std_common.h"
#include <stdio.h>
static TS_Status s(TS_VM*v,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e,const char*name,const char*seq){if(ts_std_argc(v,n,0,0,e,name)!=TS_OK)return TS_ERR_INVAL;return ts_std_ret_str(r,seq,e);}
static TS_Status clear(TS_VM*v,void*u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){(void)u;return s(v,a,n,r,e,"clear","\033[2J\033[H");}
static TS_Status reset(TS_VM*v,void*u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){(void)u;return s(v,a,n,r,e,"reset","\033[0m");}
static TS_Status cursor(TS_VM*v,void*u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){int64_t row,col;char b[64];(void)u;if(ts_std_argc(v,n,2,2,e,"cursor")!=TS_OK||ts_std_int(&a[0],&row,e,"cursor")!=TS_OK||ts_std_int(&a[1],&col,e,"cursor")!=TS_OK)return TS_ERR_INVAL;if(row<1||col<1||row>9999||col>9999)return ts_std_inval(e,"cursor: coordinates must be 1..9999");snprintf(b,sizeof b,"\033[%lld;%lldH",(long long)row,(long long)col);return ts_std_ret_str(r,b,e);}
static TS_Status toggle(TS_VM*v,void*u,const TS_Value*a,size_t n,TS_Value*r,TS_Error*e){(void)u;return s(v,a,n,r,e,"toggle", "\033[?25h");}
static const TS_FuncDef f[]={{"clear",clear,NULL},{"reset",reset,NULL},{"cursor",cursor,NULL},{"show_cursor",toggle,NULL}};
const TS_Module ts_std_termbox_module=TS_MODULE_INIT("std.termbox",f);
