# Termscript 0.4 / native ABI v2 migration

This is a versioned breaking release toward the proposed language, **not a claim
that its full roadmap is implemented**. Existing integer syntax and arithmetic
remain valid. Read `IMPLEMENTATION.md` for the current feature boundary.

## Native extensions and embedders

- Rebuild every native module with `TS_NATIVE_ABI_VERSION == 2`. ABI-v1 modules
  are rejected before their callbacks are called. Do not edit their version
  field without rebuilding and reviewing value handling.
- The legacy `DT_TermModule` adapter remains unversioned: its callbacks must
  also be rebuilt/reviewed to handle or reject `DT_TERM_FLOAT`. The `TS_Module`
  ABI-version check does not version that older adapter interface.
- `TS_FLOAT` / `DT_TERM_FLOAT` are new value tags, appended after the existing
  tags. Their payload is `as.real`, an IEEE-754 binary64 `double`. Module
  descriptor/callback shapes and existing tag values are unchanged.
- Use `ts_value_make_float`, `ts_value_parse_float` or `dt_rt_mkfloat`; every
  value must start nil before constructors/free. Non-finite inputs fail without
  replacing an existing payload. Successful callbacks returning NaN, infinity
  or an unknown value tag are rejected and their return slot is cleared.
- `ts_value_parse_float` accepts a complete finite decimal spelling, up to 256
  bytes, with no surrounding whitespace, hexadecimal syntax, NaN or infinity.
  Conversion and rendering use the C numeric locale without changing the
  caller's process/thread locale. Failed parse leaves the destination unchanged.
- CMake consumers inherit the math-library dependency; manual standalone links
  now also require `-lm` on POSIX systems, in addition to their existing parser,
  thread and optional backend dependencies.
- `termscript::value(double)` and `termlib::term_value::floating(double)` reject
  non-finite values. The Termlib C++ wrapper now correctly releases both its
  heap-allocated `DT_TermValue` storage and its payload. Its adopting pointer
  constructor requires malloc/calloc-owned storage, **not a stack or borrowed
  pointer**. After `release()`, callers must release the payload and free that
  storage themselves. `script_vm::set` transfers a converted payload copy,
  not the wrapper allocation.

## Numeric syntax and semantics

**Runnable**, tested across VM, compiled C, QBE and the hybrid Jiterati backend:

```termscript
fn half(n: float) -> float { return n / 2.0 }
print(half(5))                 # 2.5
print(1.25e2 + 0.5)            # 125.5
print(int(-3.75))              # -3 (truncate toward zero)
print(float("2.5e1"))          # 25.0
```

Decimal literals require digits on both sides of a decimal point, or an
exponent (`1e3`). Hexadecimal float literals and suffixes are unsupported.
Floating literals outside the finite binary64 range, including nonzero values
that underflow to zero, fail during front-end conversion. Representable
subnormals are accepted. Unary minus preserves signed zero.

Integer-only operations keep their checked int64 behavior, including integer
division. An arithmetic operation with a float promotes numeric operands to
binary64; large integers may round during that promotion. Comparisons **do not**
round int64 operands to float: for example `9007199254740993` is greater than,
not equal to, `9007199254740992.0`. Numeric equality accepts mixed int/float
operands; booleans remain distinct. `min`/`max` preserve the selected operand's
type and choose the left operand on ties.

Division/remainder by either signed zero and non-finite arithmetic results are
errors. Floating `%` uses C `fmod` semantics (sign follows the dividend), matching
the existing integer remainder convention. Arithmetic underflow follows the
host's binary64 arithmetic and can produce zero/subnormals. Tests use the normal
round-to-nearest floating-point environment. Bitwise operations, shifts, indices,
exit codes and native APIs requiring integers do **not** implicitly accept floats.

`float(...)` and `int(...)` are built-in conversions. `G:float` / `G:int` are their
explicit native-call spellings; user functions must not use these two names.
`int` accepts only numeric values in int64 range;
`float` accepts numbers or decimal strings. Float text uses 17 significant digits
and retains `.0` for integral floats, so `0.1` may render as
`0.10000000000000001`. Zero and negative zero are both false.

Function type annotations remain documentation, not a new static type checker.
Jiterati uses shared runtime helpers for floats; this does not claim native
floating-point machine-code generation by that experimental JIT.

## JSON, TOML and YAML

JSON/TOML fractions no longer silently truncate into int64 values. They retain
finite binary64 values through lookup, subtree cloning and stringify. JSON
number syntax is now strict: leading zeros, missing fractional/exponent digits,
overflow and nonzero underflow-to-zero are rejected. TOML NaN/infinity are
rejected explicitly instead of being converted through an undefined integer
cast. TOML's numeric locale is isolated while calling the bundled tomlc99 API.
YAML's existing block-scalar coercion now recognizes finite decimal/exponent
values; quote them to retain text. Nonnumeric plain strings remain strings.

Code relying on fractional values being integers must explicitly call `int`:

```termscript
use json
const document = json.parse("{\"count\": 2.75}")
const count = int(json.get(document, "count"))
print(count)                  # 2
```

The three adapters' `type` operations report `"float"` for floating scalars.
`std.test.eq` remains type-sensitive; use numeric language equality when an
int/float comparison is intended.

Parse adapters still return nil for malformed documents, but allocation/system
failures now propagate instead of being mistaken for malformed input or null
values. Failed string appends cannot silently truncate JSON/YAML data. TOML's
pinned API reports some allocation failures through diagnostic prefixes; the
adapter recognizes the verified prefixes and preserves the underlying message.
JSON Unicode surrogate pairs now retain supplementary characters instead of
replacing them. Unpaired surrogates and escaped NUL are rejected explicitly;
the native text ABI still cannot represent embedded NUL bytes.

## Still outside this release

Structs/enums/impl, pattern matching, first-class ranges/for iteration, closures,
generic type checking, interpolation, destructuring, indexing/method protocols,
`?`/`catch`, defer and structured scheduling are not implemented by this change.
The large application examples remain illustrative. Native modules remain
trusted code, not a sandbox. Non-Linux runtime portability and non-default
floating-point exception/rounding modes have not been verified.
