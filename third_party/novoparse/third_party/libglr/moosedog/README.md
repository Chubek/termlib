# Moosedog — parser-generator frontend for libglr

Moosedog reads a `.grm` specification (`Entrypoint` + `AST` + `Lexical` +
`Syntactic` blocks, see `examples/JSON/JSON.grm` and
`examples/ANSI-C/ANSI-C.grm`) and emits a libglr-backed C parser:
`<Base>.ast.h`, `<Base>.lexer.h`, `<Base>.parser.h`, `<Base>.parser.c`
(plus optional build files and a `Moosedog.h` copy).

## Layout

| Directory | Contents |
| --- | --- |
| `bin/` | `moosedog` CLI (`moosedog_main.c`) |
| `parser/` | `.grm` model + hand-written file parser |
| `lexer/` | `Lexical` block validation, C escaping |
| `entrypoint/` | `Entrypoint` interpretation (`Config`, `ATN`, `Optparse`, pipeline) + `Moosedog.c` runtime |
| `emitter/` | C artifact generation |
| `rewrite/` | `ImportRewriter` → `rewritelib/*.grl` bridge |
| `disamb/` | `ImportDisamb` → `disambstd` hook bridge |
| `plugin/` | `dlopen` frontend-plugin loader |
| `stdplugin/ebnf-frontend/` | EBNF (`*.ebnf`) → `.grm` translator, also a loadable plugin |
| `confuse/` + `confuse-prelude/` | `${VAR}` expansion, cache-dir resolution, default config |
| `lua/` + `lua-prelude/` | Lua `Queries` validation (`moosedog.lua` helpers) |
| `stdext/` | `LanguageType()`/`URL()`/`GetLicense()`/`Allocators()`/`RecoveryMode()`/`IntRange()` |
| `gpp-prelude/` | GPP spec-prelude macros |
| `libsh-prelude/` | shell helpers (`moosedog.sh`) |
| `templates/` | `@BASE@` emitter templates (documentation of output shape) |
| `cmake/` | `MoosedogHelpers.cmake` (`moosedog_add_grammar()`) |
| `addons/` | vim / neovim / sublime `.grm` highlighting |
| `include/` | `Moosedog.h` runtime, `Moosedog-Plugin.h` plugin ABI |
| `examples/` | JSON + ANSI-C specifications (the implementation reference) |

## Build (wired into the top-level CMake)

```sh
cmake -S . -B build -DBUILD_MOOSEDOG=ON
cmake --build build --target moosedog
ctest --test-dir build -L moosedog
```

`BUILD_MOOSEDOG=ON` (default) adds `moosedog`, `moosedog_lib`, and the
`moosedog_ebnf` plugin to the build plus four `moosedog` ctest entries
(`--check` + full emission for both examples). Disable with
`-DBUILD_MOOSEDOG=OFF`.

## Pattern dialect

Token `Pattern`s are matched by libglr's scannerless engine, which
compiles POSIX extended regular expressions. The frontend normalizes
two common PCRE-isms on emission so specs written against PCRE2 (both
bundled examples select `MainLexer = "PCRE2"`) still generate working
parsers:

- `\xHH` becomes the literal byte (`[^\x00-\x1f]` really excludes
  controls instead of excluding the letter `x`; `\x00-` maps to
  `\x01-` since NUL cannot occur in text input).
- `(?:...)` becomes `(...)` (group numbering is irrelevant to matching).

Anything else POSIX cannot honor — lookarounds, `\p` classes,
`\d`/`\s`/`\w` shorthands — is a validation error naming the token.
`Literal = Yes` spellings are raw text and skip all regex checks.

## Use

```sh
# Validate only:
moosedog --check moosedog/examples/JSON/JSON.grm

# Generate (auto-runs gpp with a sibling Support.gpp when present):
moosedog --output-dir out --basename JSON \
    moosedog/examples/JSON/JSON.grm

# Then build the generated parser against libglr:
cc -std=c11 -Iinclude -Iout out/JSON.parser.c build/libglr.a \
    build/libsfsexp.a -pthread -o json_parser
```

Flag parity with the example `Optparse` blocks: `--emit-makefile`,
`--emit-cmakefile`, `--debug`, `--no-rewrite`, `--max-errors`,
`--no-cache`, `--cache-dir`, `--strict-unicode`/`--no-strict-unicode`,
`--no-atn`, `--lookahead-depth`, `--alt-preprocessor`, `--define`,
plus `--plugin` (extra frontend `.so`) and native `.ebnf` inputs.
