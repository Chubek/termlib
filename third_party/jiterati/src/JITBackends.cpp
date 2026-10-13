/** @file src/JITBackends.cpp
 *  @brief Backend factory for the easy JIT session API.
 *
 *  Host targets use their hand-written executable backends; every other
 *  `third_party/isa` target routes to the generic ISA backend (assembly
 *  emission; executable lowering reports a descriptive error).
 */
#include "Jiterati-JIT.hpp"

#include "BE/AMD64/Backend.hpp"
#include "BE/ISA/IsaBackend.hpp"

namespace jiterati::jit {

std::unique_ptr<Backend> make_backend(std::string_view target) {
    if (target == "amd64") return std::make_unique<be::AMD64Backend>();
    if (auto isa = be::isa::create_isa_backend(target)) return isa;
    return nullptr;
}

} // namespace jiterati::jit
