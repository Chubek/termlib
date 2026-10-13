#include "Jiterati-Bytecode.hpp"
#include "Jiterati-Native.hpp"
#include "BE/AMD64/Backend.hpp"

#include <filesystem>
#include <iostream>

int main() {
    // Native pipeline: JIT-compile, archive with machine code, bundle with a
    // runtime stub, reload, and execute — the same flow `jiterati-cli run`
    // performs on .jbc bundles.
    jiterati::Module module("native_bundle_example");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));

    auto image = jiterati::bytecode::BytecodeModule::from_module(module, "amd64");
    jiterati::be::AMD64Backend backend;
    image.functions.front().code = jiterati::native::compile_to_bytes(*fn, backend);

    namespace fs = std::filesystem;
    std::string dir = (fs::temp_directory_path() / "jiterati-native-example").string();
    fs::create_directories(dir);
    jiterati::bytecode::bundle(image, dir + "/add2.run");

    auto reloaded = jiterati::bytecode::load(dir + "/add2.run");
    std::cout << jiterati::bytecode::describe(reloaded);
    std::cout << "add2(20, 22) = "
              << jiterati::bytecode::execute(reloaded, "add2", {20, 22}) << "\n";
    std::cout << "bundle: " << dir << "/add2.run\n";
    return 0;
}
