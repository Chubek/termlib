/* std.keyremap: bounded, allocation-safe text/key sequence replacement. */
#include "keyremp.h"
#include "common/ts_std_common.h"
#include <string>
#include <cstring>
namespace {
constexpr size_t LIMIT = 1024u * 1024u;
TS_Status replace(TS_VM *vm, void *, const TS_Value *a, size_t n, TS_Value *r, TS_Error *e) {
  if (ts_std_argc(vm,n,3,3,e,"keyremap.apply") != TS_OK) return TS_ERR_INVAL;
  const char *in,*from,*to;
  if (ts_std_str(&a[0],&in,e,"keyremap.apply") != TS_OK || ts_std_str(&a[1],&from,e,"keyremap.apply") != TS_OK || ts_std_str(&a[2],&to,e,"keyremap.apply") != TS_OK) return TS_ERR_INVAL;
  size_t il=std::strlen(in), fl=std::strlen(from), tl=std::strlen(to);
  if (il>LIMIT || fl>LIMIT || tl>LIMIT || fl==0) { ts_error_set(e,TS_ERR_LIMIT,0,0,"keyremap: invalid or oversized mapping"); return TS_ERR_LIMIT; }
  std::string out; out.reserve(il);
  for (size_t p=0;p<il;) { if (p+fl<=il && std::memcmp(in+p,from,fl)==0) { if (out.size()>LIMIT-tl) { ts_error_set(e,TS_ERR_LIMIT,0,0,"keyremap: output exceeds 1 MiB"); return TS_ERR_LIMIT; } out.append(to,tl); p+=fl; } else { out.push_back(in[p++]); } }
  return ts_std_ret_strn(r,out.data(),out.size(),e);
}
TS_Status identity(TS_VM *vm, void *, const TS_Value *a,size_t n,TS_Value*r,TS_Error*e) { if(ts_std_argc(vm,n,1,1,e,"keyremap.identity")!=TS_OK)return TS_ERR_INVAL; const char*s; if(ts_std_str(&a[0],&s,e,"keyremap.identity")!=TS_OK)return TS_ERR_INVAL; if(std::strlen(s)>LIMIT){ts_error_set(e,TS_ERR_LIMIT,0,0,"keyremap: input exceeds 1 MiB");return TS_ERR_LIMIT;} return ts_std_ret_str(r,s,e); }
const TS_FuncDef funcs[]={{"apply",replace,nullptr},{"remap",replace,nullptr},{"identity",identity,nullptr}};
}
extern "C" const TS_Module ts_std_keyremap_module = TS_MODULE_INIT("std.keyremap",funcs);
