/* Scannerless GLR descriptor builder over the vendored NovoParse C runtime.
 * Handles own descriptors/parser; copied DOM trees survive reuse and close.
 * Grammar construction and parsing require external serialization per handle.
 * Grammars are trusted: the underlying GLR engine has no allocation/time
 * budget.
 */
#ifndef TS_STD_GRAMMAR_H
#define TS_STD_GRAMMAR_H
#include <termscript/termscript.h>
#ifdef __cplusplus
extern "C"
{
#endif
  extern const TS_Module ts_std_grammar_module;
#ifdef __cplusplus
}
#endif
#endif
