/* Bounded byte-oriented parsing expression grammars, with ordered choice.
 * Compiled handles own their grammar; parse returns an independent DOM tree.
 * No callbacks, native code generation or hidden grammar registry. Serialize
 * handle access against close; independent parses keep all state local.
 */
#ifndef TS_STD_PEG_H
#define TS_STD_PEG_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_peg_module;
#ifdef __cplusplus
}
#endif
#endif
