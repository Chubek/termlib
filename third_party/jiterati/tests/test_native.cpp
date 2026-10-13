/** @file tests/test_native.cpp
 *  @brief Native pipeline tests: JIT bytes, bundle round-trip, execution.
 */
#include "Jiterati.hpp"
#include "Jiterati-Bytecode.hpp"
#include "Jiterati-Native.hpp"
#include "BE/AMD64/Backend.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s at line %d\n", #cond, __LINE__); \
        std::exit(1); \
    } \
} while (0)

namespace {

namespace fs = std::filesystem;

std::string temp_name(std::string const& stem) {
    static unsigned n = 0;
    return (fs::temp_directory_path() /
            (stem + "-" + std::to_string(++n) + ".tmp"))
        .string();
}

void test_code_size() {
    jiterati::Module module("native_bytes");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    jiterati::be::AMD64Backend backend;
    auto compiled = backend.compile_function(*fn);
    CHECK(compiled->code_size() == 5);
    CHECK(compiled->size() >= 5);
    auto bytes = jiterati::native::compile_to_bytes(*fn, backend);
    CHECK(bytes.size() == 5);
    CHECK(bytes[0] == 0x89 && bytes[4] == 0xc3);
}

void test_execute_round_trip() {
    jiterati::Module module("native_exec");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    auto image = jiterati::bytecode::BytecodeModule::from_module(module, "amd64");
    jiterati::be::AMD64Backend backend;
    image.functions.front().code = jiterati::native::compile_to_bytes(*fn, backend);
    CHECK(jiterati::bytecode::execute(image, "add2", {3, 4}) == 7);
    CHECK(jiterati::bytecode::execute(image, "add2", {-10, 5}) == -5);
    bool threw = false;
    try {
        jiterati::bytecode::execute(image, "missing", {});
    } catch (...) {
        threw = true;
    }
    CHECK(threw);
    // Save / load / bundle / load preserves the executable payload.
    std::string jbc = temp_name("native");
    std::string run = temp_name("native") + ".run";
    jiterati::bytecode::save(image, jbc);
    jiterati::bytecode::bundle_file(jbc, run);
    CHECK(jiterati::bytecode::is_bundle(run));
    auto reloaded = jiterati::bytecode::load(run);
    CHECK(reloaded.functions.size() == 1);
    CHECK(jiterati::bytecode::execute(reloaded, "add2", {20, 22}) == 42);
    {
        std::ifstream head(run, std::ios::binary);
        std::string first(2, '\0');
        head.read(first.data(), 2);
        CHECK(first == "#!");
    }
    std::error_code ec;
    fs::remove(jbc, ec);
    fs::remove(run, ec);
}

void test_native_helpers() {
    // GAS translation of Jiterati Intel-syntax assembly.
    std::string gas = jiterati::native::to_gas("global add\nadd:\n  ret\n", "amd64");
    CHECK(gas.find(".globl add") != std::string::npos);
    CHECK(gas.find(".intel_syntax noprefix") != std::string::npos);
    // Unknown targets degrade to a descriptive log, never a crash.
    auto report = jiterati::native::assemble_text("no-such-target", "ret");
    CHECK(!report.ok);
    CHECK(!report.log.empty());
    // ELF .text extraction rejects non-ELF input.
    CHECK(jiterati::native::extract_text_section({0x00, 0x01}).empty());
#if !defined(_WIN32)
    auto ok = jiterati::native::run_file("/bin/true");
    CHECK(ok.exit_code == 0);
    auto fail = jiterati::native::run_file("/bin/false");
    CHECK(fail.exit_code != 0);
    auto echo = jiterati::native::run_file("/bin/echo", {"hello-native"});
    CHECK(echo.exit_code == 0);
    CHECK(echo.output.find("hello-native") != std::string::npos);
#endif
}

} // namespace

int main() {
    test_code_size();
    test_execute_round_trip();
    test_native_helpers();
    std::printf("test_native passed\n");
    return 0;
}
