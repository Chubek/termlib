# Chapter 19: Termscript C Backend

Compilation parses and validates Termscript source, emits ordinary C statements,
and stores the generated unit until the next compile or VM destruction.  The C
backend consumes the same NovoParse AST as the VM, QBE, and Jiterati adapters.

## Syntax accepted

The C backend supports the current runnable syntax documented in Chapter 17:

- `const` declarations and assignments;
- expression statements;
- `if ... do ... [else ...] end`;
- `while ... do ... end`;
- scalar literals, variables, `or`, references, and native calls;
- both whitespace calls (`G:puts value`) and parenthesized calls
  (`G:puts(value)`).

Nested expression arguments are not emitted because they are not part of the
front-end syntax yet.  Bind the nested result first.

## Generated code contract

Generated C calls `ts_rt_*` for standalone mode or `dt_rt_*` for Termlib mode.
Statements become C control flow; source text is not embedded for runtime
parsing.  Termscript-side modules loaded with constant-string `G:import` are
resolved at compile time and inlined where possible.  Native modules are not
embedded; they resolve through the VM registry at runtime.

## CLI

```sh
# Emit C.
termscript -c -o program.c script.tsc

# Compile and link through the configured C compiler.
termscript -o program script.tsc

# Select compiler and static link mode.
termscript --cc clang -o program script.tsc
termscript --static -o program script.tsc
```

`--backend c` is accepted as the explicit spelling of the default backend.  The
C backend is also selected whenever `--backend` is omitted.

## Reference

See `ts_vm_compile_to_c`, `ts_vm_compile_backend`,
`dt_termscript_compile_to_c`, and `dt_termscript_compile_backend` in
`termscript/termscript.h` and `termlib.h`.
