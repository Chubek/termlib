#include "Jiterati-JIT.hpp"
#include "Jiterati-DSL.hpp"
#include "Jiterati-SIMD.hpp"

#include <iostream>

int main() {
    // C++-native DSL: describe the function, then run it through the easy JIT.
    jiterati::dsl::DslModule mod("easy_jit_example");
    auto add = mod.function<int(int, int)>("add2");
    auto sum = add.param(0) + add.param(1);
    add.entry().ret(sum.emit(add.entry()));

    jiterati::jit::Session session;
    jiterati::Module& owned = session.create_module("easy_jit_example");
    jiterati::Function* fn = owned.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));

    auto callable = session.compile_typed<int(int, int)>(*fn, "amd64");
    std::cout << "add2(20, 22) = " << callable(20, 22) << "\n";
    std::cout << "simd: " << jiterati::simd::describe_cpu() << "\n";
    return 0;
}
