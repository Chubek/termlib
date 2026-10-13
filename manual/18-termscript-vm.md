# Chapter 18: Termscript VM and C API

Termscript exposes two C surfaces:

- `termscript/termscript.h`: standalone VM, values, native modules, text
  backends, and backend-selected execution.
- `termlib.h`: `DT_TermVM`, Termlib-flavored values, and adapters over the
  standalone VM.

## Ownership

- `TS_VM *` and `DT_TermVM *` are transferred to the caller and freed with the
  matching `*_free` function; cleanup accepts `NULL`.
- `TS_Value`/`DT_TermValue` string payloads are owned by the value and released
  with `ts_value_free`/`dt_termvalue_free`.
- Native module tables and names are borrowed and must outlive every VM that can
  call them.
- Captured run output returned through `char **output` is allocated with
  `malloc`; callers free it with `free`.
- Compiled text returned by compile APIs is borrowed until the next compile on
  the same VM or VM destruction.

## Compile options

Use option initializers before calling the generic backend dispatcher:

```c
TS_CompileOptions options;
ts_compile_options_init(&options);
options.backend = TS_BACKEND_QBE;
const char *ir = ts_vm_compile_backend(vm, source, &options, &error);
```

The Termlib adapter mirrors this with `DT_TermscriptCompileOptions` and
`dt_termscript_compile_backend`.  Nonzero flags and unsupported backends fail
without modifying the previous compiled buffer.

`*_compile_backend` is for text/AOT output only.  It accepts:

- `TS_BACKEND_C` / `DT_TERMSCRIPT_BACKEND_C`: generated C with `main`.
- `TS_BACKEND_QBE` / `DT_TERMSCRIPT_BACKEND_QBE`: QBE IL entry point
  `ts_qbe_run(vm, error)`.

It rejects Jiterati because Jiterati executes code; it does not expose a textual
Termscript backend format.

## Backend availability

`ts_backend_available`/`dt_termscript_backend_available` report implemented
backend adapters:

- `c`: available
- `qbe`: available
- `jiterati`: available only when the experimental AMD64 execution adapter is
  built and its startup probe succeeds

Jiterati is execution-only.  Use `ts_vm_run_backend` or
`dt_termscript_run_backend`; `*_compile_backend` rejects it because no textual
Jiterati output format is exposed.  The current adapter JITs checked i32
arithmetic/comparison calls in the `G` module and executes the rest of the
runnable subset through explicit runtime glue.  Native executable code is still
not a sandbox.

## Backend-selected execution

```c
char *output = NULL;
TS_Error error;
ts_error_clear(&error);

TS_Status status = ts_vm_run_backend(
    vm,
    "const n = G:add(40, 2); G:puts(n);",
    TS_BACKEND_JITERATI,
    &output,
    &error);
```

Rules:

- `TS_BACKEND_JITERATI` / `DT_TERMSCRIPT_BACKEND_JITERATI` execute through the
  experimental Jiterati adapter when available.
- `TS_BACKEND_C` and `TS_BACKEND_QBE` return `UNSUPPORTED` from run-backend APIs;
  compile them with `*_compile_backend` or use the command-line driver.
- Output ownership is unchanged: `char **output` receives a `malloc` allocation
  on success and the caller frees it with `free`.
- A backend must report `UNSUPPORTED` for missing lowering rather than silently
  falling back to the VM.

## Command-line driver

Default execution uses the VM:

```sh
termscript script.tsc
termscript - < script.tsc
```

C backend:

```sh
# Emit C to stdout or a file.
termscript -c script.tsc
termscript -c -o script.c script.tsc

# Compile and link an executable through the C compiler.
termscript -o program script.tsc
termscript --cc clang -o program script.tsc
termscript --static -o program script.tsc
```

QBE backend:

```sh
# Emit QBE IL.
termscript --backend qbe -c -o script.ssa script.tsc

# Build a native executable on supported platforms.
termscript --backend qbe -o program script.tsc
termscript --backend qbe --static -o program script.tsc
```

Jiterati backend:

```sh
termscript --backend jiterati script.tsc
```

Jiterati is execution-only.  The driver rejects `--backend jiterati -c` and
`--backend jiterati -o program`.
