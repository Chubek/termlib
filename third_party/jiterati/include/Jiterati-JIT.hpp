/** @file Jiterati-JIT.hpp
 *  @brief Ergonomic JIT session API for Jiterati.
 *
 *  The core `jiterati::JIT` class compiles one `Function` against one
 *  `Backend`.  This header layers a session model on top of it:
 *
 *  - @ref Session owns modules, selects a backend per target, keeps
 *    compiled code alive, and resolves cross-function calls;
 *  - @ref JitFunction wraps a typed function pointer with lifetime
 *    management;
 *  - free helpers cover the common single-shot cases
 *    (@ref compile_and_run, @ref compile_callable).
 *
 *  Header-only, C++17, depends only on Jiterati.hpp/Jiterati-BE.hpp.
 *  Targets without an executable lowering (everything except the host
 *  integer fast path) report a descriptive error instead of crashing.
 */
#ifndef JITERATI_JIT_HPP_INCLUDED
#define JITERATI_JIT_HPP_INCLUDED

#include "Jiterati.hpp"
#include "Jiterati-BE.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jiterati::jit {

/** Error raised when a JIT operation cannot be completed. */
class JitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Typed handle to JIT-compiled code. Keeps the backing allocation alive. */
template <typename Sig>
class JitFunction;

template <typename Ret, typename... Args>
class JitFunction<Ret(Args...)> {
public:
    using FnPtr = Ret (*)(Args...);

    JitFunction() = default;
    JitFunction(FnPtr ptr, std::shared_ptr<CompiledFunction> owner)
        : ptr_(ptr), owner_(std::move(owner)) {}

    bool valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return valid(); }

    FnPtr get() const { return ptr_; }

    Ret operator()(Args... args) const {
        if (!ptr_) throw JitError("call of empty JitFunction");
        return ptr_(args...);
    }

private:
    FnPtr ptr_ = nullptr;
    std::shared_ptr<CompiledFunction> owner_;
};

/** Select a backend instance for a target id. Defined in BE translators. */
std::unique_ptr<Backend> make_backend(std::string_view target);

/** Backend id matching the current host CPU, or "wasm" as portable fallback. */
inline std::string host_target() {
#if defined(__x86_64__) || defined(_M_X64)
    return "amd64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    return "rv64";
#else
    return "wasm";
#endif
}

/** A session owns modules and every function compiled from them. */
class Session {
public:
    Session() = default;

    explicit Session(std::string default_target)
        : default_target_(std::move(default_target)) {}

    /// Create a module owned by this session.
    Module& create_module(std::string name = "jit") {
        modules_.emplace_back(std::make_unique<Module>(std::move(name)));
        return *modules_.back();
    }

    /// Compile `fn` for `target` (default: session default or host).
    /// Returns shared ownership of the executable allocation.
    std::shared_ptr<CompiledFunction> compile(Function& fn,
                                              std::string_view target = {}) {
        std::string id = target.empty()
            ? (default_target_.empty() ? host_target() : default_target_)
            : std::string(target);
        std::unique_ptr<Backend> backend = make_backend(id);
        if (!backend) throw JitError("unknown JIT target: " + id);
        std::unique_ptr<CompiledFunction> owned =
            JIT::create_default().compile(fn, *backend);
        if (!owned) throw JitError("backend " + id + " refused function " + fn.name());
        std::shared_ptr<CompiledFunction> shared(std::move(owned));
        compiled_.push_back(shared);
        return shared;
    }

    /// Compile and type the result in one call.
    template <typename Sig>
    JitFunction<Sig> compile_typed(Function& fn, std::string_view target = {}) {
        std::shared_ptr<CompiledFunction> owned = compile(fn, target);
        auto* raw = reinterpret_cast<typename JitFunction<Sig>::FnPtr>(owned->entry());
        if (!raw) throw JitError("backend returned null entry for " + fn.name());
        return JitFunction<Sig>(raw, std::move(owned));
    }

    /// Build a trivial function from a C++ callable description and compile it.
    /// `build` receives (Module&, Function*) and must fill the entry block.
    template <typename Sig, typename Build>
    JitFunction<Sig> define_and_compile(std::string name,
                                        Build&& build,
                                        std::string_view target = {}) {
        Module& mod = create_module("jit_" + name);
        Function* fn = mod.template create_function<Sig>(std::move(name));
        build(mod, fn);
        return compile_typed<Sig>(*fn, target);
    }

    std::size_t compiled_count() const { return compiled_.size(); }
    void reset() {
        modules_.clear();
        compiled_.clear();
    }

private:
    std::string default_target_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<std::shared_ptr<CompiledFunction>> compiled_;
};

/** One-shot: compile `fn` and return a typed callable. */
template <typename Sig>
inline JitFunction<Sig> compile_callable(Function& fn, std::string_view target = {}) {
    Session session;
    return session.compile_typed<Sig>(fn, target);
}

/** One-shot: compile `fn` and immediately invoke it. */
template <typename Ret, typename... Args>
inline Ret compile_and_run(Function& fn,
                            std::string_view target,
                            Args... args) {
    auto callable = compile_callable<Ret(Args...)>(fn, target);
    return callable(args...);
}

} // namespace jiterati::jit

#endif // JITERATI_JIT_HPP_INCLUDED
