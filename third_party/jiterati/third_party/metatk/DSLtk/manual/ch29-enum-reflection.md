# Chapter 29 — Enum Reflection

`DSLtk.hpp` embeds the complete public header API of
[magic_enum v0.9.8](https://github.com/Neargye/magic_enum/tree/v0.9.8): core
reflection, flags, utility functions, fusion, constexpr switch dispatch,
containers, stream operators, and formatting. It remains a self-contained
C++20 header; no magic_enum installation or link dependency is required.

## Namespaces and feature composition

Reflection functions and traits are exposed directly in `dsl::` with using
declarations. These declarations preserve the upstream overloads, template
parameters, return types, `constexpr`, and `noexcept` behavior.

The entire API is also available through `dsl::enum_reflection`, which aliases
`::magic_enum`. Namespace aliases expose the other public facilities:

| DSLtk namespace | Upstream namespace |
| --- | --- |
| `dsl::customize` | `magic_enum::customize` |
| `dsl::containers` | `magic_enum::containers` |
| `dsl::bitwise_operators` | `magic_enum::bitwise_operators` |
| `dsl::iostream_operators` | `magic_enum::iostream_operators` |
| `dsl::istream_operators` | `magic_enum::istream_operators` |
| `dsl::ostream_operators` | `magic_enum::ostream_operators` |

`dsl::EnumReflection` is a feature-presence marker, like `dsl::PatternMatch`:

```cpp
struct Tokens : dsl::DSL<Tokens, dsl::EnumReflection, dsl::PatternMatch> {};
static_assert(dsl::HasFeature<Tokens, dsl::EnumReflection>);
```

The functions are free functions; a DSL base class is optional.

## Reflection and conversion

```cpp
enum class Token { Number = -1, Identifier = 2, End = 7 };

static_assert(dsl::enum_count<Token>() == 3);
static_assert(dsl::enum_name(Token::Identifier) == "Identifier");
static_assert(dsl::enum_cast<Token>("identifier", dsl::case_insensitive)
              == Token::Identifier);
static_assert(dsl::enum_value<Token, 1>() == Token::Identifier);
```

Values, names, and entries are ordered by underlying numeric value, not by
declaration order. Each distinct numeric value has one reflected name.

| API | Result |
| --- | --- |
| `enum_type_name<E>()` | Unqualified enum type name |
| `enum_count<E>()` | Number of reflected values |
| `enum_values<E>()` | Reference to a static constexpr array of values |
| `enum_names<E>()` | Reference to a static constexpr array of names |
| `enum_entries<E>()` | Reference to a static constexpr array of `(value, name)` pairs |
| `enum_value<E>(index)`, `enum_value<E, Index>()` | Value at an index |
| `enum_name(value)`, `enum_name<Value>()` | Enumerator name |
| `enum_integer(value)`, `enum_underlying(value)` | Underlying integer |
| `enum_index(value)` | Optional index |
| `enum_index<Value>()` | Compile-time checked index |
| `enum_cast<E>(integer)` | Optional enum, only for a reflected value |
| `enum_cast<E>(name, predicate = {})` | Optional enum, exact or predicate-based name lookup |
| `enum_contains(value)`, `enum_contains<E>(integer/name)` | Reflected membership |
| `enum_reflected(value)`, `enum_reflected<E>(integer)` | Whether the value lies in the search interval |

`case_insensitive` performs ASCII case-insensitive comparison. A binary
character predicate may be supplied instead. Unknown runtime values produce
empty names, empty optionals, or `false`, as appropriate.

`enum_value<E>(index)` requires a valid index. Its compile-time overload checks
the index at compilation. `enum_name<Value>()` and `enum_index<Value>()` require
a named/reflected value and diagnose invalid template arguments at compilation.

### Traits and reflection modes

- `is_scoped_enum<T>`, `is_scoped_enum_v<T>`;
- `is_unscoped_enum<T>`, `is_unscoped_enum_v<T>`;
- `underlying_type<T>`, `underlying_type_t<T>`;
- `enum_constant<Value>`: `std::integral_constant` holding an enum value;
- `Enum<T>`: upstream enum-type wrapper;
- `is_magic_enum_supported`: compiler support indicator;
- `dsl::is_flags<T>`, `dsl::is_flags_v<T>`: whether an enum uses flags reflection;
  these DSLtk additions are false for non-enum types.

Where supported by an upstream function's signature, an explicit reflection
mode can be selected with `dsl::as_flags<>` or `dsl::as_common<>`:

```cpp
constexpr auto ordinary_values = dsl::enum_values<Access, dsl::as_common<>>();
```

## Customization

The default inclusive numeric search interval is `[-128, 127]`. Change the
global interval with `MAGIC_ENUM_RANGE_MIN` and `MAGIC_ENUM_RANGE_MAX` before
including `DSLtk.hpp`, or specialize an individual enum's range:

```cpp
enum class Opcode { Load = 1000, Store = 1010 };

template <> struct dsl::customize::enum_range<Opcode> {
  static constexpr int min = 1000;
  static constexpr int max = 1010;
};
```

`enum_name<Value>()` does not scan the interval and can name enumerators outside
it. Per-enum search spans must remain smaller than `UINT16_MAX`.

Name customization supports replacement names and exclusion:

```cpp
template <>
constexpr dsl::customize::customize_t
dsl::customize::enum_name<Opcode>(Opcode value) noexcept {
  switch (value) {
    case Opcode::Load: return "load";
    case Opcode::Store: return dsl::customize::invalid_tag;
    default: return dsl::customize::default_tag;
  }
}
```

`customize::enum_type_name<E>()` customizes a type name in the same way.
`customize::enum_range<E>::prefix_length` trims a prefix from discovered names.

Ranges, flag status, and prefix trimming can also be declared through ADL in
the enum's namespace:

```cpp
namespace lexer {
enum class Kind { TOK_Number = 1000, TOK_End = 1001 };

inline auto magic_enum_define_range_adl(Kind) {
  return dsl::customize::adl_info().minmax<1000, 1001>().prefix<4>();
}
}
static_assert(dsl::enum_name(lexer::Kind::TOK_Number) == "Number");
```

Declare all specializations and ADL hooks before the first reflection of the
affected enum. `dsl::customize` is a namespace alias: use qualified
specializations as above, or open `namespace magic_enum::customize`.

All upstream `MAGIC_ENUM_*` configuration hooks are retained, including
`MAGIC_ENUM_CONFIG_FILE`, custom optional/string/string-view aliases,
`MAGIC_ENUM_ENABLE_HASH`, `MAGIC_ENUM_ENABLE_HASH_SWITCH`, assertion controls,
and container exception controls. Configuration must be consistent across
translation units. The original per-header guards also permit including
external magic_enum v0.9.8 headers before or after `DSLtk.hpp`.

## Flags and operators

```cpp
enum class Access : unsigned { None = 0, Read = 1, Write = 2, Execute = 4 };
template <> struct dsl::customize::enum_range<Access> {
  static constexpr bool is_flags = true;
};
using namespace dsl::bitwise_operators;

auto permissions = Access::Read | Access::Write;
auto name = dsl::enum_flags_name(permissions); // "Read|Write"
auto parsed = dsl::enum_flags_cast<Access>("read,WRITE", ',', dsl::case_insensitive);
```

| API | Operation |
| --- | --- |
| `enum_flags_name(value, separator = '|')` | Join the names of set bits |
| `enum_flags_cast<E>(integer)` | Validate and cast a nonzero combination |
| `enum_flags_cast<E>(name, separator = '|', predicate = {})` | Parse names separated by a delimiter |
| `enum_flags_contains(value/integer/name, ...)` | Validate a combination |
| `enum_flags_test(flags, mask)` | Test whether all nonzero mask bits are present |
| `enum_flags_test_any(lhs, rhs)` | Test whether any bits overlap |

Flags reflection enumerates named individual power-of-two bits, including
high bits of unsigned underlying types. Zero and named composite constants
are excluded from the flags value list. Zero is not a valid flag combination;
unknown bits also fail validation. Global numeric range macros do not limit
the flags scan.

`bitwise_operators` contains `~`, `|`, `&`, `^`, `|=`, `&=`, and `^=`. Import
these operators explicitly where required; they are enabled for all enum types.

## Constexpr iteration, dispatch, fusion, and navigation

```cpp
dsl::enum_for_each<Token>([](auto constant) {
  constexpr Token value = decltype(constant)::value;
  // Instantiate enum-specific code for each value.
});

auto integer = dsl::enum_switch([](auto constant) {
  return dsl::enum_integer(decltype(constant)::value);
}, Token::Identifier, -999);
```

- `enum_for_each<E>(fn)` invokes `fn` with each `enum_constant`. Void-returning
  functions produce no result; homogeneous results produce an array, and
  heterogeneous results produce a tuple.
- `enum_switch(fn, value)` dispatches a runtime value into a compile-time
  constant. An unmatched value or unsupported handler receives a
  default-constructed result (or no action for `void`). The three-argument
  overload accepts an explicit fallback result. An explicit result type and
  reflection mode are supported by the upstream overloads.
- `enum_fuse(a, b, ...)` combines at least two reflected enum values into an
  optional, type-safe enum discriminator suitable for `switch`/`case`.
  All arguments must be reflected; the combined representation must fit in
  `std::uintmax_t`.
- `enum_next_value(value, steps = 1)` and `enum_prev_value(value, steps = 1)`
  return optionals when moving through the sorted value sequence. They return
  empty optionals for unknown values or out-of-bounds movement.
- `enum_next_value_circular(value, steps = 1)` and
  `enum_prev_value_circular(value, steps = 1)` wrap around the sequence. Their
  starting value must be reflected.

## Enum-indexed containers

`dsl::containers` provides the full upstream containers, iterators, comparison
policies, and helper functions:

- `array<E, T, Index = ...>`: fixed-size storage indexed by enum values, with
  iterators, checked `at`, tuple access, comparisons, and `fill`/`swap`;
- `make_array<E>(values...)`, `to_array<E>(c_array)`, and `get<Value>(array)`;
- `bitset<E, Index = ...>`: constexpr enum-indexed bits, proxy references,
  bitwise operations, `set`/`reset`/`flip`, queries, string and integer
  conversions; `raw_access` distinguishes raw bit positions from enum values;
- `set<E, Compare = std::less<E>>`: a compact set with enum-keyed
  insertion, erasure, lookup, bounds queries, and bidirectional iteration;
- `name_less<>`, `name_greater<>`, `name_less_case_insensitive`, and
  `name_greater_case_insensitive`: name-based comparison policies;
- `default_indexing<E>` and `comparator_indexing<Compare>`: index mapping
  policies for arrays and bitsets.

```cpp
constexpr auto weights = dsl::containers::make_array<Token>(1, 10, 0);
static_assert(weights[Token::Identifier] == 10);

dsl::containers::bitset<Token> seen{Token::Number, Token::End};
dsl::containers::set<Token, dsl::containers::name_less<>> tokens_by_name;
tokens_by_name.insert(Token::Identifier);
```

Container positions use the reflected sequence, not the enum's raw integer.
Checked operations retain upstream exception behavior; `MAGIC_ENUM_NO_EXCEPTION`
selects aborting container checks.

## Streams and formatting

```cpp
using dsl::iostream_operators::operator<<;
using dsl::iostream_operators::operator>>;
std::cout << Token::Identifier; // "Identifier"
```

Insertion prints reflected names, flag combinations, or the underlying integer
for unknown values. It also accepts optional enum values. Extraction parses a
name or flag combination and sets `failbit` on failure, leaving the destination
value intact.

If the standard library provides `<format>` and `__cpp_lib_format`, enum
formatters are enabled automatically, including when `DSLtk.hpp` is included
first. They support string formatting specifications and numeric fallback for
unknown values. `{fmt}` support is enabled when its headers are included before
`DSLtk.hpp`; it remains optional.

```cpp
#include <fmt/format.h> // Optional external formatter, included first.
#include "DSLtk.hpp"
// fmt::format("{}", value), std::format("{}", value)
```

## Compiler behavior and provenance

Reflection uses upstream compiler-signature parsing (`__PRETTY_FUNCTION__` or
`__FUNCSIG__`). It supports GCC, Clang, and MSVC on the versions supported by
magic_enum; the surrounding DSLtk API requires C++20. Forward-declared enums
cannot be reflected. Alias naming is compiler-dependent, and aliases do not
add distinct values. Large scan intervals increase constexpr compilation work.
See the pinned upstream [reference](https://github.com/Neargye/magic_enum/blob/v0.9.8/doc/reference.md)
and [limitations](https://github.com/Neargye/magic_enum/blob/v0.9.8/doc/limitations.md)
for complete overload and compiler details.

The embedded block retains all upstream MIT copyright and permission notices.
`scripts/update-magic-enum.py` recursively expands the public headers, preserves
their include guards, adds optional `<format>` detection, and defers
container-specific string requirements until those operations are used. This
allows core reflection's minimal custom string aliases to coexist with the
included container declarations. Its source digest
pins the exact v0.9.8 headers. To reproduce or verify the block from an upstream
v0.9.8 checkout:

```sh
python3 DSLtk/scripts/update-magic-enum.py /path/to/magic_enum
python3 DSLtk/scripts/update-magic-enum.py /path/to/magic_enum --check
```

The integration tests can be built independently of the other MetaTk libraries:

```sh
cmake -S DSLtk/tests -B build/dsltk-enums
cmake --build build/dsltk-enums
ctest --test-dir build/dsltk-enums --output-on-failure
```

The MetaTk build also enables these tests with `BUILD_TESTING=ON` and the example
with `BUILD_EXAMPLES=ON`.
