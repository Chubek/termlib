/** @file Lua/JiteratiBinding.cpp
 *  @brief Lua/Sol2 binding registration.
 *
 *  Registers the `ljiterati` module (macro registry, instruction emitters,
 *  runtime metadata) on a sol2 state when Sol2/Lua are enabled; otherwise
 *  compiles to a dependency-free stub so the core library never requires Lua.
 */
#include "../include/Jiterati.hpp"
#include "../include/Jiterati-Macro.hpp"

#include <cstdio>

namespace jiterati {

/// Register ljiterati helpers on an opaque Lua state pointer.
/// The void* ABI avoids hard-depending on sol2/Lua headers here; the
/// typed overload in Jiterati-Macro.hpp performs the real registration.
void register_ljiterati_stub(void* /*state*/) {}

} // namespace jiterati
