# Chapter 17: Termscript Language

For the user and embedding reference, see the [Termscript Manual](termscript.md).
The parser source is `termscript/Termscript.ng`, compiled by NovoParse/GLR into
the common AST consumed by VM, C, QBE and Jiterati. The implementation status is
recorded in `termscript/IMPLEMENTATION.md`; a proposed example is not a language
specification or evidence that its application runs.

## Shipped syntax

- Statements are separated by newlines or semicolons; the last simple statement
  may appear directly before `}` or EOF. `#` comments run to the end of a line.
- `const`/`let` create immutable bindings; `var` and assignment create/update
  mutable bindings. `+=`, `-=`, `*=`, `/=` and `%=` are supported.
- Values include nil, booleans, checked int64, finite binary64 floats, NUL-free
  strings and reference-counted native handles. Integers can be decimal or hex;
  floats use decimal fractions/exponents. Strings have the supported C-like
  escapes, including nonzero `\xHH`; embedded NUL is rejected.
- Expressions support parentheses, checked arithmetic, comparisons, bitwise
  operators/shifts and unary negation/not. `and`/`or` short-circuit and return an
  operand. Arguments and operands evaluate left-to-right once.
- Calls support `M:f(...)`, module dot calls, ordinary named functions and the
  historical whitespace form. Parenthesized calls accept nested expressions and
  trailing commas. `&name` retains the existing reference syntax.
- `if`/`else` and `while` accept brace bodies or legacy `do`/`then`/`end` forms.
  Top-level named functions support positional arguments, recursion and return.
  Function frames see their own locals and top-level globals, not caller locals.
  `main()` or `main(args)` runs after initialization; its nil/int result controls
  exit status. There are no closures or mutable lexical captures yet.
- `use` binds registered `std.*` modules. List and map literals use the existing
  native adapters; they require the standard library, not an implicit new value
  representation or collection method protocol.
- Section declarations, aliases/defines, `config`, `lifetime` and the legacy flow
  operators are implemented and covered by section/cleanup conformance tests.
  `~>` observes failure, `->` follows the error observer and `=>` observes
  success. Error observers **do not suppress the original failure**; they are not
  the proposed `catch`/`?` semantics.

Type annotations currently document intent; they are not statically enforced.
For precedence and exact lifetime/operator contracts, consult the implementation
status and grammar rather than inferring behavior from illustrative applications.

## Runnable example

The numeric conformance fixture tests this syntax across all four backends:

```termscript
fn half(n: float) -> float { return n / 2.0 }
var total = 0.0
while total < 1.0 { total += 0.25 }
print(half(5) + total)          # 3.5
```

Floating arithmetic promotes numeric operands to binary64; integer-only division
still returns an integer. Mixed comparisons do not round int64 values before
comparing. NaN/infinity and division/remainder by zero fail explicitly. See the
[0.4 migration guide](../termscript/MIGRATING-0.4.md) for numeric semantics and
the required native ABI-v2 rebuild.

## Implementation boundary

Structs/enums/impl, pattern matching, first-class ranges/for loops, closures,
generic type checking, interpolation, destructuring, collection indexing/method
protocols, `?`/`catch`, defer and structured scheduling remain roadmap work.
The large `coedit`, `flashbench`, `kmsterm`, `tmx` and `tsh` applications are
illustrative, not runnable feature claims.

C and QBE emit actual control flow over the compiled-code runtime, without
embedding source to parse at startup. Jiterati is a hybrid execution backend:
checked i32 operations can be JIT-compiled, while floats and other operations
use shared runtime helpers. This is not a whole-program or floating-point JIT.
