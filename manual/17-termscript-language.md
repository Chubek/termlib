# Chapter 17: Termscript Language

For the complete user and embedding reference, see the
[Termscript Manual](termscript.md).

Termscript is parsed from `termscript/Termscript.ng` by the vendored
NovoParse/GLR pipeline.  This chapter describes the shipped executable syntax:
the subset accepted by that grammar and lowered through the shared AST used by
the VM, C, QBE, and Jiterati adapters.

## Runnable subset

Whitespace and comments are layout.  Comments start with `#` and run to the end
of the line.  Runnable programs currently consist of these statements:

- `const name = expr;`
- `name = expr;`
- `expr;`
- `if expr do stmt* [else stmt*] end`
- `while expr do stmt* end`

Expressions currently include:

- variables: `name`
- references: `&name`
- integers, strings, `true`, `false`, `nil`
- fallback: `lhs or rhs` (`or` short-circuits)
- native calls:
  - whitespace form: `G:puts value`
  - parenthesized form: `G:puts(value)`

Parenthesized call arguments are scalar values or references separated by
commas.  A trailing comma is accepted.  Empty calls use `M:f()`; whitespace-form
calls with no arguments use `M:f;`.  Nested expression arguments are not yet part
of the runnable subset, so write:

```termscript
const sum = G:add(40, 2);
G:puts(sum);
```

not `G:puts(G:add(40, 2));`.

String escapes are the C-like subset implemented by the parser: `\n`, `\t`,
`\r`, `\"`, `\\`, `\e`, `\a`, `\b`, `\f`, `\v`, and nonzero `\xHH` bytes.
NUL bytes are rejected.

## Runnable examples

```termscript
# Parenthesized calls.
const Gmod = G:load("G") or G:die("G missing");
const answer = G:add(40, 2);
G:puts(answer);

# Whitespace calls remain valid.
G:puts "done";
```

```termscript
counter = 0;
while G:lt(counter, 3) do
  counter = G:add(counter, 1);
  if G:eq(counter, 2) do
    G:puts("two");
  else
    G:puts(counter);
  end
end
```

## Illustrative examples

The section-oriented files in `termscript/examples` are design inputs, not
runnable conformance tests.  Constructs such as `@alias`, `@define`,
`.interface`, `.section`, `lifetime`, `config`, `~>`, `->`, and `=>` remain
reserved for the next semantic-IR stage.  They must not be described as working
until parsing, ownership, cleanup, diagnostics, and all claimed backends are
implemented together.

## Backend contract

The VM, C, QBE, and Jiterati adapters consume the same AST.

- VM: default tree-walking execution.
- C: emits real C control flow over `ts_rt_*`/`dt_rt_*`; it does not embed
  Termscript source for runtime parsing.
- QBE: emits a `ts_qbe_run` entry point with the same runtime-status convention;
  the driver can assemble and link it on supported native platforms.
- Jiterati: execution/JIT adapter, not a text emitter.  The tested slice JITs
  checked i32 `G:add`, `G:sub`, `G:mul`, `G:div`, `G:mod`, `G:eq`, `G:lt`,
  `G:gt`, `G:le`, and `G:ge`, while using runtime glue for control flow,
  variables, strings, imports, and native module calls.  Unsupported
  machine-code lowering fails explicitly.
