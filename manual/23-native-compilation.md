# Native compilation with QBE and Keystone

Termscript supports direct ahead-of-time compilation through the bundled
`third_party/qbe` and `third_party/keystone`:

```sh
termscript --backend qbe -o program script.tsc
./program

# Inspect the QBE intermediate representation:
termscript --backend qbe -c -o script.ssa script.tsc

# Fully static linking (requires static dependencies):
termscript --backend qbe --static -o program script.tsc
```

The native executable backend currently targets **Linux x86-64 (System V
ABI, ELF64)**. CMake enables `TERMSCRIPT_NATIVE` by default on that platform.
Disable it with `-DTERMSCRIPT_NATIVE=OFF` to omit QBE and Keystone from the
driver build. Other platforms retain the VM and C backend. QBE IL emission
is available independently of the native toolchain; its value layout follows
the compiler host's runtime ABI, so it is not a cross-compilation interface.

The driver lowers the parsed AST directly into QBE control flow and calls
to the compiled-code runtime. QBE performs optimization, register allocation,
and instruction selection. Keystone assembles those instructions, local
branches, and literals. Termscript packages the resulting bytes into an ELF
relocatable object, using external-call veneers with linker relocations.

`--cc CC` (or `$CC`, default `cc`) compiles a small entry-point wrapper and
links the object with the Termscript runtime and native libraries. Script
statements are compiled by QBE. QBE and Keystone are needed during compilation,
not when running the executable. The bundled QBE executable is installed as
`termscript-qbe` alongside the driver.

The native backend supports literals, variables, calls, assignment, constants,
short-circuit `or`, conditionals, and loops. It shares compile-time constant
`G:import` expansion with the C backend; dynamic imports retain their existing
runtime behavior. Native modules resolve through the runtime registry. Output,
runtime errors, and step-budget accounting follow the VM. Owned temporary
values are released on both successful and failed execution.

## Embedding and tooling

`dt_termscript_compile_to_qbe(vm, source, error)` emits Termlib-flavored IL.
`ts_vm_compile_to_qbe(vm, source, dt_mode, error)` exposes the shared emitter;
use `dt_mode = false` for the standalone runtime. Both return a borrowed string
valid until the next compile on that VM or its destruction. The usual
single-threaded-per-VM contract applies. Compilation parses but does not execute
the source.

The IL exports `ts_qbe_run(vm, error)`, returning the appropriate status type.
Embedding callers initialize the error, create/configure a VM, call the compiled
function, inspect its status and captured output, and release the VM. IL emission
does not emit `main`. The command-line driver supplies that wrapper automatically.

Existing invocations keep their behavior: scripts run in the VM by default,
`-o program` uses the C backend, and `-c` emits C. Select `--backend qbe`
explicitly to use the QBE native backend.

## Backend flag matrix

| Invocation | Meaning |
| --- | --- |
| `termscript script.tsc` | Run with the VM. |
| `termscript -c [-o file.c] script.tsc` | Emit C. |
| `termscript -o program script.tsc` | Compile/link through the C backend. |
| `termscript --backend c ...` | Explicit C backend spelling. |
| `termscript --backend qbe -c -o file.ssa script.tsc` | Emit QBE IL. |
| `termscript --backend qbe -o program script.tsc` | Build native executable through QBE/Keystone. |
| `termscript --backend jiterati script.tsc` | Execute through the Jiterati JIT adapter. |

`--cc CC` selects the C compiler used for driver-managed links.  `--static`
applies to link steps only.  Jiterati is execution-only: combining
`--backend jiterati` with `-c` or `-o` is rejected.

## Jiterati execution backend

The Jiterati path is not a text or object emitter.  It executes the runnable
Termscript subset through `dt_termscript_run_backend` and JITs checked i32
arithmetic/comparison calls in the `G` module on builds where the vendored AMD64
backend is available.  Native executable memory is used; this is not a sandbox.
