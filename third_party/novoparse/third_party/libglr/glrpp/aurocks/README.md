# Aurocks — `.g` grammar frontend for GLRpp

Aurocks reads a small EBNF specification (`examples/JSON.g`) and lowers it
to a GLRpp/libglr parser: either live `glrpp::Grammar` objects (library)
or generated C++ artifacts — parser, runtime wrapper, AST header, Makefile
(`aurocks` CLI).

```sh
aurocks --check glrpp/aurocks/examples/JSON.g
aurocks --output-dir out glrpp/aurocks/examples/JSON.g
```

## Dialect

```text
grammar JSON

%start JSON
%language C++
%parser JSONParser.cpp
%runtime JSONParserRuntime.hpp
%AST JSONParserAST.hpp
%build Makefile
%ATN on
%lexer SCANNERLESS

JSON = Object | Array | String | Number | "true" | "false" | "null"
String = "\"" StringChar* "\""
layout = [\ \t\n\r]
```

- `grammar NAME` header (required, first statement).
- Rules: `Name = alt | alt ...`, spanning lines; a new rule starts at
  `Ident =`, so alternatives may break lines freely. `|` separates
  alternatives; juxtaposition is sequencing.
- Elements: rule references (`Member`), double/single-quoted literals
  (`"{"`, `"\\u"`), character classes (`[0-9]`, `[\u0020-\u0021]`),
  groups (`("e" | "E")`), and `?`/`*`/`+` suffixes on any of them.
- Escapes in literals and classes: `\\ \" \' \/ \b \f \n \r \t \v \0`
  `\xHH` `\uHHHH` (UTF-8 encoded in literals) plus `\c` for any other `c`.
- Comments: `//...` and `/*...*/`.
- `layout` is the reserved skip rule: terminal-only alternatives matched
  between structural symbols. `%skip /[...]/` synthesizes it for
  single-class skips. A grammar without either is whitespace-exact.
- Directives: `%start` (required), `%language` (`C++` only), `%parser`,
  `%runtime`, `%AST`, `%build` (output basenames), `%ATN on|off`,
  `%lexer SCANNERLESS`. Unknown `%...` directives are retained as metadata
  (same policy as `scripts/aurocks.pl`) but otherwise ignored.

The Perl `scripts/aurocks.pl` dialect (`%{ %}`, `%%`, `rule: ... ;`) is a
different language; `.g` files in that shape are rejected with a diagnostic.

## Lowering

1. Reference validation, then a lexical fixpoint: a rule is lexical when
   built only from terminals and lexical rules.
2. EBNF desugar to explicit productions with synthetic rules:
   `X?` -> `P ::= X | %empty`, `X*` -> `P ::= P X | %empty`,
   `X+` -> `P ::= P X | X`, `(...)` -> one production per alternative.
   Repetition bases keep their lexicality, so `"a"*` stays tight while
   structural `B*` admits layout.
3. Layout: a nullable `layout_star` nonterminal is interleaved between
   siblings of non-lexical productions only (never inside lexical rules,
   `layout` itself, or at production edges), plus a `Start$start`
   wrapper for document edges. Each inter-symbol boundary has exactly one
   gap owner, so layout introduces no accepted-forest ambiguity
   (`glr_parser_stack_count == 1` on the JSON examples).
4. Classes become scannerless POSIX ERE patterns on dedicated terminals;
   literals become exact-match terminals.

## Patterns and locales

In UTF-8 locales (notably `en_US.UTF-8`) glibc collation makes ERE ranges
over punctuation/space unreliable, and ranges over multibyte bytes fail
to compile. Aurocks therefore emits ASCII classes as alternations of
single atoms (no ranges, no brackets). A high tail covering exactly
`U+0080..U+FFFF` becomes a range-free negated class over enumerated
ASCII (`[^]<0x01..0x7F minus ] and -->]`); partial non-ASCII coverage is
a lowering error. Classes containing `NUL` are rejected (patterns are
NUL-terminated C strings in libglr).

## Generated artifacts

`<Base>.parser.cpp` rebuilds the lowered grammar through the GLRpp API
and exposes `parse_<start>()`-equivalent `JSONParser::parse()` returning
`dsl::Result<glrpp::ParseTree, std::string>`; `<Base>.runtime.hpp` wraps
it in an owning `<Name>Parser` class; `<Base>.ast.hpp` builds an
untyped AST with S-expression output; `Makefile` compiles a demo
(`-DAUROCKS_DEMO_MAIN`) that prints the S-expression of its file
argument. Generated code needs `-I` for `GLRpp.hpp` and `<glr/*.h>` plus
`libglr` (baked into the generated `Makefile` for the configuring build
tree; override `AUROCKS_CXXFLAGS`/`AUROCKS_LDFLAGS` otherwise).

## Layout of this directory

| Path | Contents |
| --- | --- |
| `Aurocks.hpp` / `Aurocks.cpp` | `SpecParser`, `GrammarBuilder`, `Emitter`, `compile_file` |
| `cli.cpp` | `aurocks` driver (`--check`, `--output-dir`) |
| `CMakeLists.txt` | `aurocks` lib, `aurocks` tool, unit + end-to-end ctest entries |
| `examples/JSON.g` | Reference specification (implementation contract) |
| `tests/test_aurocks.cpp` | Catch2 suite: spec, lowering, patterns, live parse, emission |
| `tests/data/ok.json`, `bad.json` | Demo fixtures |
