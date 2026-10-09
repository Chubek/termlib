# NovoParse

NovoParse is a GLR parser generator written in D and backed by the vendored
[`libglr`](third_party/libglr). It compiles `.ng` grammars to a deterministic
S-expression intermediate representation, then uses S-expression profiles to
emit parsers for **C, C++, D, and Python**.

The generated source contains grammar descriptors. The native runtime applies
requested grammar rewrites, constructs the GLR parse table, and builds labeled
syntax trees from accepted input. Ambiguous grammars and overlapping terminal
patterns are supported.

## Build and test

Requirements: CMake 3.16+, C/C++ compilers, and a D compiler (`ldc2`, `dmd`, or
`gdc`). Python 3 is needed when building the test suite. Dependencies are
vendored; building requires no network access.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

This produces `build/novoparse` and `build/libnovoparse.so` on Linux. To build
just the D compiler frontend, use `dub build`; its unit tests run with `dub test`.
Set `-DBUILD_TESTING=OFF` for a build without the tests.

## Generate a parser

```sh
mkdir -p generated
build/novoparse check examples/JSON.ng
build/novoparse compile examples/JSON.ng -o generated/JSON.sexp
build/novoparse generate generated/JSON.sexp --profile Python -o generated/JSON.py
```

Generation also accepts `.ng` directly. Output defaults to stdout. `--profile`
accepts a built-in name or the path to a custom `.sexp` profile.

Use the generated Python parser:

```sh
PYTHONPATH=runtime:generated NOVOPARSE_LIBRARY="$PWD/build/libnovoparse.so" python3 - <<'PY'
from JSON import JSONParser

with JSONParser() as parser:
    tree = parser.parse('{"answer": 42, "items": [true, null]}')
    print(tree)
PY
```

`parse()` returns a labeled syntax-tree dictionary, including terminal lexemes
and byte spans. The tree remains usable after the parser is reused or closed.
Errors include a byte offset and one-based line/byte-column location.

## Grammar example

```text
%grammar Calculator;
%start expression;
%skip /[ \t\r\n]+/;
%token NUMBER /[0-9]+/;
%left "+";
%left "*";

expression: expression "+" expression => Add
          | expression "*" expression => Multiply
          | "(" expression ")" => Group
          | NUMBER => Number;
```

Rules support alternatives, grouped expressions, `?`, `*`, `+`, and empty
productions. Directives configure named terminals, trivia, precedence,
associativity, ambiguity policies, and `libglr` grammar rewrites.

See [the grammar reference](docs/grammar.md) and
[profiles and runtime integration](docs/profiles.md) for the complete syntax,
the IR format, and C/C++/D usage.

## Layout

- `source/`: D grammar reader, validation, EBNF lowering, IR, profiles, emitter,
  and command-line frontend.
- `profiles/`: target-language S-expression profiles.
- `runtime/`: native C runtime and C++, D, and Python adapters.
- `include/`: source-tree C forwarding header and D interface.
- `examples/`: JSON, arithmetic, and deliberately ambiguous grammars.
- `tests/`: frontend, native API, generated-language, and end-to-end tests.
- `third_party/`: vendored native dependencies and their licensing notices.

## Install

```sh
cmake --install build --prefix "$HOME/.local"
```

The CLI is installed in `bin/`, the shared runtime in `lib/`, native headers in
`include/novoparse/`, and profiles/adapters in `share/novoparse/`. The built-in
profiles are embedded in the CLI. Python users can add the installed runtime
directory to `PYTHONPATH` and specify the shared library through
`NOVOPARSE_LIBRARY`.

NovoParse's own code is MIT-licensed; vendored libraries retain their licenses.
