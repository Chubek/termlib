/** @file tests/test_new_features.cpp
 *  @brief Coverage for the NEW ADDITIONS workstream.
 *
 *  Exercises: ISA backend registry + emission for every new target, easy JIT
 *  session API, C++-native DSL, SIMD batch math, DocIR kernels + serializers,
 *  Keystone/Keystone-mapping + ELF link, bytecode archive round-trip +
 *  runtime stub, Capstone/termlib disassembly view.
 */
#include "Jiterati.hpp"
#include "Jiterati-JIT.hpp"
#include "Jiterati-DSL.hpp"
#include "Jiterati-SIMD.hpp"
#include "Jiterati-DocIR.hpp"
#include "Jiterati-Asm.hpp"
#include "Jiterati-Bytecode.hpp"
#include "Jiterati-Disasm.hpp"
#include "BE/ISA/IsaBackend.hpp"
#include "DocIR/Kernels.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s at line %d\n", #cond, __LINE__); \
        std::exit(1); \
    } \
} while (0)

namespace {

void test_isa_registry() {
    auto const& targets = jiterati::be::isa::all_isa_targets();
    CHECK(targets.size() >= 20);
    for (char const* id : {"arm32", "mips64", "ppc64", "s390x", "sparc64",
                           "loongarch64", "bpf", "amdgpu", "nvptx", "spirv",
                           "tricore", "avr", "m68k", "hexagon"}) {
        CHECK(jiterati::be::isa::find_isa_target(id) != nullptr);
    }
    CHECK(jiterati::be::isa::find_isa_target("nope") == nullptr);
}

void test_isa_emit() {
    jiterati::Module module("isa_emit");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    for (char const* id : {"arm32", "rv64", "riscv32", "mips32", "ppc64",
                           "s390x", "sparc64", "wasm"}) {
        auto backend = jiterati::be::isa::create_isa_backend(id);
        CHECK(backend != nullptr);
        CHECK(backend->target_id() == id);
        std::string asm_text = backend->emit_assembly(*fn);
        CHECK(asm_text.find(id) != std::string::npos);
    }
    auto compiler = jiterati::be::isa::create_isa_ir_compiler("arm32");
    CHECK(compiler != nullptr);
    CHECK(compiler->target() == "arm32");
}

void test_easy_jit() {
    jiterati::jit::Session session;
    jiterati::Module& module = session.create_module("easy");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    auto callable = session.compile_typed<int(int, int)>(*fn, "amd64");
    CHECK(callable.valid());
    CHECK(callable(3, 4) == 7);
    CHECK(callable(-10, 5) == -5);
    CHECK(session.compiled_count() == 1);
}

void test_dsl() {
    jiterati::dsl::DslModule mod("dsl_demo");
    auto fn = mod.function<int(int, int)>("add2");
    auto x = fn.param(0);
    auto y = fn.param(1);
    jiterati::dsl::Expr sum = x + y;
    jiterati::dsl::Expr scaled = sum * fn.constant_i32(2);
    fn.entry().ret(scaled.emit(fn.entry()));
    std::string dump = mod.dump();
    CHECK(dump.find("add2") != std::string::npos);
    auto pred = jiterati::dsl::pred_and(jiterati::dsl::is_vreg(),
                                        jiterati::dsl::pred_not(jiterati::dsl::is_const()));
    jiterati::Value v = jiterati::Value::vreg(3);
    CHECK(pred(v));
    jiterati::Module fallback("dsl_fn");
    CHECK(jiterati::dsl::define_i32_add(fallback) != nullptr);
}

void test_simd() {
    std::vector<float> a = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<float> b = {8, 7, 6, 5, 4, 3, 2, 1};
    std::vector<float> out(8, 0), mul(8, 0), fma(8, 0), root(8, 0);
    jiterati::simd::batch_add(a.data(), b.data(), out.data(), 8);
    jiterati::simd::batch_mul(a.data(), b.data(), mul.data(), 8);
    jiterati::simd::batch_fma(a.data(), b.data(), b.data(), fma.data(), 8);
    jiterati::simd::batch_sqrt(a.data(), root.data(), 8);
    for (float v : out) CHECK(v == 9.0f);
    CHECK(mul[0] == 8.0f);
    CHECK(fma[0] == 1.0f * 8.0f + 8.0f);
    CHECK(std::fabs(root[3] - 2.0f) < 1e-5f);
    CHECK(jiterati::simd::vector_width<float>() >= 1);
    CHECK(!jiterati::simd::describe_cpu().empty());
    CHECK(jiterati::simd::policy_width(jiterati::simd::VectorPolicy::Fixed128, 4) == 4);
}

void test_docir() {
    using namespace jiterati::docir;
    using namespace jiterati::docir::kernels;
    Document catalog = kernel_backend_catalog();
    std::string xtas = serialize(catalog, "xtas");
    CHECK(xtas.find("arm32") != std::string::npos);
    CHECK(xtas.find("xt:as") != std::string::npos);
    Document passes = kernel_pass_catalog();
    CHECK(serialize(passes, "html").find("<html") != std::string::npos);
    Document cli = kernel_cli_reference();
    CHECK(cli.to_xtas().find("disasm") != std::string::npos);
    Document article = kernel_article("Hello", {"Body paragraph."});
    CHECK(article.title() == "Hello");
    Stylesheet theme = Stylesheet::default_theme();
    CHECK(!theme.to_css().empty());
    std::vector<FunctionSummary> fns = {{"add2", "i32 (i32, i32)", 1, 2}};
    Document ref = kernel_module_reference("demo", fns);
    CHECK(ref.to_xtas().find("add2") != std::string::npos);
}

void test_asm_link() {
    CHECK(jiterati::asmlink::ks_arch_for_target("amd64") ==
          jiterati::asmlink::KsArch::X86);
    CHECK(jiterati::asmlink::ks_arch_for_target("rv64") ==
          jiterati::asmlink::KsArch::RISCV);
    CHECK(jiterati::asmlink::ks_arch_for_target("nope") ==
          jiterati::asmlink::KsArch::Unknown);
    std::vector<std::uint8_t> text = {0x89, 0xf8, 0x01, 0xf0, 0xc3};
    auto elf = jiterati::asmlink::link_elf("amd64", text);
    CHECK(elf.size() > text.size());
    CHECK(elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F');
    auto result = jiterati::asmlink::assemble("amd64", "mov eax, edi");
    CHECK(!result.ok || !result.bytes.empty());
}

void test_bytecode() {
    jiterati::Module module("bc_demo");
    jiterati::Function* fn = module.create_function<int(int, int)>("add2");
    jiterati::Block* entry = fn->create_block("entry");
    entry->ret(entry->add(entry->arg(0), entry->arg(1)));
    auto image = jiterati::bytecode::BytecodeModule::from_module(module, "amd64");
    CHECK(!image.empty());
    CHECK(image.entry == "add2");
    std::string blob = jiterati::bytecode::encode_records(image.functions);
    auto decoded = jiterati::bytecode::decode_records(blob);
    CHECK(decoded.size() == 1);
    CHECK(decoded.front().name == "add2");
    std::string stub = jiterati::bytecode::emit_stub_c("demo.jbc", "add2");
    CHECK(stub.find("add2") != std::string::npos);
    CHECK(!jiterati::bytecode::describe(image).empty());
}

void test_disasm() {
    std::vector<std::uint8_t> bytes = {0x89, 0xf8, 0x01, 0xf0, 0xc3};
    auto insns = jiterati::disasm::disassemble("amd64", bytes, 0x1000);
    CHECK(!insns.empty());
    CHECK(!jiterati::disasm::render_table(insns).empty());
    CHECK(!jiterati::disasm::render_view(insns, false).empty());
    CHECK(!jiterati::disasm::render_view(insns, true).empty());
}

} // namespace

int main() {
    test_isa_registry();
    test_isa_emit();
    test_easy_jit();
    test_dsl();
    test_simd();
    test_docir();
    test_asm_link();
    test_bytecode();
    test_disasm();
    std::printf("test_new_features passed\n");
    return 0;
}
