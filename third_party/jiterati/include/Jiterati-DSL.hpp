/** @file Jiterati-DSL.hpp
 *  @brief C++-native DSL for building Jiterati IR without boilerplate.
 *
 *  Design mirrors `third_party/metatk/DSLtk/DSLtk.hpp` idioms (pipeline
 *  operator|, composable predicates, lazy expression templates) but is
 *  implemented in C++17 so it compiles in this project's default mode.
 *  When MetaTk DSLtk is available under C++20, its Pipeline/Operators
 *  mixins interoperate through the thin adapters in DSLtk.hpp.
 *
 *  Example:
 *  @code
 *  jiterati::dsl::DslModule mod("demo");
 *  auto add = mod.function<int(int,int)>("add2");
 *  auto& entry = add.entry();
 *  auto x = add.param(0), y = add.param(1);
 *  entry.ret(entry.add(x, y));
 *  jiterati::jit::Session jit;
 *  // move module into session, compile, call ...
 *  @endcode
 */
#ifndef JITERATI_DSL_HPP_INCLUDED
#define JITERATI_DSL_HPP_INCLUDED

#include "DSLtk.hpp"
#include "Jiterati.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jiterati::dsl {

/** Lazy integer/float expression node evaluated against a Block. */
class Expr {
public:
    using Emitter = std::function<Value(Block&)>;

    explicit Expr(Emitter emit) : emit_(std::move(emit)) {}
    explicit Expr(Value v) : emit_([v](Block&) { return v; }) {}

    Value emit(Block& block) const { return emit_(block); }

    friend Expr operator+(Expr const& a, Expr const& b) {
        return Expr([a, b](Block& block) {
            return block.add(a.emit(block), b.emit(block));
        });
    }
    friend Expr operator-(Expr const& a, Expr const& b) {
        return Expr([a, b](Block& block) {
            return block.sub(a.emit(block), b.emit(block));
        });
    }
    friend Expr operator*(Expr const& a, Expr const& b) {
        return Expr([a, b](Block& block) {
            return block.mul(a.emit(block), b.emit(block));
        });
    }
    friend Expr operator/(Expr const& a, Expr const& b) {
        return Expr([a, b](Block& block) {
            return block.sdiv(a.emit(block), b.emit(block));
        });
    }

private:
    Emitter emit_;
};

/** Owning wrapper around Module with fluent function builders. */
class DslModule {
public:
    explicit DslModule(std::string name = "dsl") : module_(std::move(name)) {}

    Module& get() { return module_; }
    Module const& get() const { return module_; }

    /** Wrap a raw Function with block/entry helpers. */
    class DslFunction {
    public:
        DslFunction(Function* fn) : fn_(fn) {
            entry_ = fn_->create_block("entry");
        }

        Function* get() { return fn_; }
        Block& entry() { return *entry_; }

        Expr param(std::size_t i) {
            Value v = entry_->arg(i);
            return Expr(v);
        }
        Expr constant_i32(int32_t v) {
            Function* fn = fn_;
            return Expr([fn, v](Block&) { return fn->const_i32(v); });
        }
        Expr constant_i64(int64_t v) {
            Function* fn = fn_;
            return Expr([fn, v](Block&) { return fn->const_i64(v); });
        }

        void done() {}

    private:
        Function* fn_ = nullptr;
        Block* entry_ = nullptr;
    };

    template <typename Sig>
    DslFunction function(std::string name) {
        Function* fn = module_.create_function<Sig>(std::move(name));
        return DslFunction(fn);
    }

    std::string dump() const { return module_.to_string(); }

private:
    Module module_;
};

/** Pipeline stage: value | stage(f). Mirrors dsl::pipe from DSLtk. */
template <typename F>
inline auto stage(F&& f) {
    return dsltk::pipe<std::string>(std::forward<F>(f));
}

/** Predicate combinators over IR values (Operators-style &, |, !). */
using ValuePred = std::function<bool(Value const&)>;

inline ValuePred pred_and(ValuePred a, ValuePred b) {
    return [a = std::move(a), b = std::move(b)](Value const& v) {
        return a(v) && b(v);
    };
}
inline ValuePred pred_or(ValuePred a, ValuePred b) {
    return [a = std::move(a), b = std::move(b)](Value const& v) {
        return a(v) || b(v);
    };
}
inline ValuePred pred_not(ValuePred a) {
    return [a = std::move(a)](Value const& v) { return !a(v); };
}

inline ValuePred is_vreg() {
    return [](Value const& v) { return v.is_vreg(); };
}
inline ValuePred is_const() {
    return [](Value const& v) { return v.is_const(); };
}

/** Build `ret (a+b)` for a two-parameter i32 function in one call. */
template <typename Ret, typename Lhs, typename Rhs>
inline void emit_add_ret(DslModule::DslFunction& fn, Lhs&& lhs, Rhs&& rhs);

/** Convenience: define int(int,int) add and return the function pointer. */
inline Function* define_i32_add(Module& module, std::string name = "add2") {
    Function* fn = module.create_function<int(int, int)>(std::move(name));
    Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    return fn;
}

} // namespace jiterati::dsl

#endif // JITERATI_DSL_HPP_INCLUDED
