#include "BE/Metadata.hpp"

#include <algorithm>
#include <array>

namespace jiterati::be {

namespace {

constexpr std::array<BackendDatasetMetadata, 22> metadata{{
    {"amd64", "amd64", "opcodes/i386-opc.tbl", 1557, 1007},
    {"aarch64", "aarch64", "arch/aarch64-insn.h", 42, 42},
    {"rv64", "riscv", "RISCV-Opdoes", 1072, 1037},
    {"wasm", "wasm", "arch/wasm-binary.h", 670, 623},
    {"arm32", "arm32", "third_party/isa/arm32.isa", 114, 114},
    {"riscv32", "riscv32", "third_party/isa/riscv32.isa", 199, 199},
    {"mips32", "mips32", "third_party/isa/mips32.isa", 62, 62},
    {"mips64", "mips64", "third_party/isa/mips64.isa", 84, 84},
    {"ppc32", "ppc32", "third_party/isa/ppc32.isa", 64, 64},
    {"ppc64", "ppc64", "third_party/isa/ppc64.isa", 90, 90},
    {"s390x", "s390x", "third_party/isa/s390x.isa", 70, 70},
    {"sparc64", "sparc64", "third_party/isa/sparc64.isa", 107, 107},
    {"loongarch64", "loongarch64", "third_party/isa/loongarch64.isa", 148, 148},
    {"loongarch32", "loongarch32", "third_party/isa/loongarch32.isa", 91, 91},
    {"m68k", "m68k", "third_party/isa/m68k.isa", 104, 104},
    {"avr", "avr", "third_party/isa/avr.isa", 68, 68},
    {"hexagon", "hexagon", "third_party/isa/hexagon.isa", 26, 26},
    {"bpf", "bpf", "third_party/isa/bpf.isa", 138, 138},
    {"amdgpu", "amdgpu", "third_party/isa/amdgpu.isa", 139, 139},
    {"nvptx", "nvptx", "third_party/isa/nvptx.isa", 42, 42},
    {"spirv", "spirv", "third_party/isa/spirv.isa", 873, 873},
    {"tricore", "tricore", "third_party/isa/tricore.isa", 31, 31},
}};

} // namespace

BackendDatasetMetadata const* backend_dataset_metadata_begin() { return metadata.data(); }

BackendDatasetMetadata const* backend_dataset_metadata_end() { return metadata.data() + metadata.size(); }

BackendDatasetMetadata const* find_backend_dataset_metadata(std::string_view target) {
    auto it = std::find_if(metadata.begin(), metadata.end(), [&](BackendDatasetMetadata const& item) {
        return item.target == target;
    });
    return it == metadata.end() ? nullptr : &*it;
}

} // namespace jiterati::be
