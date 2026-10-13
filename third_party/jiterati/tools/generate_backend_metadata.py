#!/usr/bin/env python3
"""Emit BackendDatasetMetadata initializers.

Lua datasets under .agents/datasets/ cover the four hand-written backends.
Every other target is described by a third_party/isa/*.isa source; for those
the operation-form count (lines starting with "op ") is reported for both
instruction and mnemonic columns.
"""
from pathlib import Path


def string_after(line: str, key: str) -> str:
    return line.split(key, 1)[1].split('"', 1)[0]


def dataset_stats(path: Path):
    arch = None
    source = None
    mnemonics = []
    instruction_count = 0
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith('arch = "'):
            arch = string_after(stripped, 'arch = "')
        elif stripped.startswith('source = "'):
            source = string_after(stripped, 'source = "')
        elif stripped.startswith('name = "'):
            instruction_count += 1
        elif stripped.startswith('mnemonic = "'):
            mnemonic = string_after(stripped, 'mnemonic = "')
            if mnemonic not in mnemonics:
                mnemonics.append(mnemonic)
    return arch, source, instruction_count, len(mnemonics)


def isa_stats(path: Path):
    arch = None
    operations = 0
    for line in path.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith("arch "):
            arch = stripped.split(None, 2)[1]
        elif stripped.startswith("op "):
            operations += 1
    return arch, f"third_party/isa/{path.name}", operations


ISA_TARGETS = {
    "arm32": "arm32.isa",
    "riscv32": "riscv32.isa",
    "mips32": "mips32.isa",
    "mips64": "mips64.isa",
    "ppc32": "ppc32.isa",
    "ppc64": "ppc64.isa",
    "s390x": "s390x.isa",
    "sparc64": "sparc64.isa",
    "loongarch64": "loongarch64.isa",
    "loongarch32": "loongarch32.isa",
    "m68k": "m68k.isa",
    "avr": "avr.isa",
    "hexagon": "hexagon.isa",
    "bpf": "bpf.isa",
    "amdgpu": "amdgpu.isa",
    "nvptx": "nvptx.isa",
    "spirv": "spirv.isa",
    "tricore": "tricore.isa",
}


def main():
    for target in ("amd64", "aarch64", "rv64", "wasm"):
        arch, source, instruction_count, mnemonic_count = dataset_stats(Path(f".agents/datasets/{target}.lua"))
        print(f'{{"{target}", "{arch}", "{source}", {instruction_count}, {mnemonic_count}}},')
    for target, filename in ISA_TARGETS.items():
        arch, source, operations = isa_stats(Path(f"third_party/isa/{filename}"))
        print(f'{{"{target}", "{arch}", "{source}", {operations}, {operations}}},')


if __name__ == "__main__":
    main()
