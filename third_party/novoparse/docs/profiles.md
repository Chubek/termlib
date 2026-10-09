# Profiles, IR, and runtime integration

## Compiler pipeline

1. The D reader parses grammar directives, rules, and EBNF expressions.
2. Compilation resolves symbols, lowers EBNF into helper productions, infers
   precedence, and validates the grammar.
3. `compile` serializes a versioned S-expression IR.
4. `generate` renders that IR using the selected profile.
5. A generated parser creates the native runtime from grammar descriptors.
   `libglr` applies rewrites and builds the parse table at parser creation.

`generate` accepts a `.ng` grammar or a `.sexp` IR file. Both routes produce the
same output. The frontend is independent of the native runtime, so `dub build`
can be used to generate source without first building the shared library.

## S-expression IR

```lisp
(novoparse
  (version 1)
  (name "Hello")
  (start 0)
  (skip none "")
  (disambiguation reject)
  (rewrites)
  (symbols
    (symbol nonterminal "document" "" 0 none)
    (symbol literal "WORD" "hello" 0 none))
  (productions
    (production 0 (body 1) "Greeting" 0 none)))
```

Symbol IDs are zero-based indices in the symbols list; production IDs are
indices in the productions list. Symbol fields are kind, name, value,
precedence, and associativity. Production fields are head, body, label,
precedence, and associativity. Empty labels select the head's name.

All eight top-level fields are required, once each. The reader rejects
unsupported versions, malformed shapes, invalid references, and duplicate
symbols. Serialization preserves declaration order and uses canonical
quoting, making IR and generated source reproducible. The Python descriptor
JSON is emitted with a fixed key order (`name`, `start`, `skip_kind`,
`skip`, `disambiguation`, `rewrites`, `symbols`, `productions`) so repeated
runs are byte-identical.

## Profile format

Profiles are data, read by the D frontend:

```lisp
(profile
  (version 1)
  (name "C")
  (extension ".h")
  (backend c)
  (template "/* {{name}} */\n{{tables}}\n"))
```

- `name` and `extension` identify the target and conventional output suffix.
- `backend` selects descriptor formatting: `c`, `d`, or `python`. The C++
  profile shares the `c` descriptor backend and wraps it in an RAII class.
- `template` supplies the target-language source around the descriptors.
- `{{name}}` expands to the grammar name.
- `{{tables}}` expands to immutable C descriptors, D descriptor construction,
  or a quoted JSON descriptor string for Python.

All fields are required; duplicate fields and unknown placeholders are errors.
The CLI writes to stdout or the explicit `-o` path; it does not choose a
filename from the extension. Custom profiles using an existing backend can be
loaded by path. A new descriptor representation requires an emitter backend
and runtime adapter. Implemented built-ins are C, C++, D, and Python.

## C

```sh
build/novoparse generate examples/JSON.ng --profile C -o generated/JSON.h
```

Include `JSON.h`, then call `JSON_create(error, sizeof error)`. Use
`np_parser_parse(parser, input, length)` and inspect `np_parser_root()` with the
node-accessor API or serialize it with `np_parser_json()`. Destroy the parser
with `np_parser_destroy()`.

```c
char error[512];
np_parser *parser = JSON_create(error, sizeof error);
if (!parser) { fprintf(stderr, "%s\n", error); return 1; }
np_status status = np_parser_parse(parser, "[1,2]", 5);
if (status == NP_SUCCESS) puts(np_parser_json(parser));
else fprintf(stderr, "%s\n", np_parser_error(parser));
np_parser_destroy(parser);
```

Link with `-lnovoparse`. Source-tree include paths are `runtime/` and the
generated-header directory. Installed headers are under `include/novoparse/`.
The runtime copies the descriptors during creation and the input during
parsing. Returned nodes, text, and JSON are borrowed until the next parse or
parser destruction. The C error enum distinguishes syntax, memory, grammar,
ambiguity, and semantic-derivation failures.

## C++

```sh
build/novoparse generate examples/Arithmetic.ng --profile C++ -o generated/Arithmetic.hpp
```

`ArithmeticParser` wraps `novoparse::Parser` with RAII and move ownership:

```cpp
ArithmeticParser parser;
std::string tree_json = parser.parse("1 + 2 * 3");
```

The result is an independent JSON string. Parsing errors throw
`novoparse::ParseError`, with status, position, line, and column. C++11 or newer
is required; link the same native runtime used by C.

## D

```sh
build/novoparse generate examples/Arithmetic.ng --profile D -o generated/Arithmetic.d
```

The generated module exposes `createParser()`:

```d
import Arithmetic : createParser;
auto parser = createParser();
scope(exit) parser.close();
auto tree = parser.parse("1 + 2 * 3");
assert(tree["name"].str == "Add");
```

Compile `runtime/NovoParse.d` with the generated module and application, adding
`-Iruntime -Igenerated -L-Lbuild -L-lnovoparse` for LDC/DMD. Set the runtime
library search path or add an rpath. `include/NovoParse.di` provides the matching
D interface for separately compiled clients. Results are `std.json.JSONValue`;
errors throw `NovoParse.ParseError`. `close()` is idempotent.

## Python

Generated modules expose a `NAMEParser` class and a convenience `parse(text,
library=None)` function. The adapter uses `ctypes`, with no external Python
dependencies. Pass `library=PATH`, set `NOVOPARSE_LIBRARY`, or install the shared
library in the system search path. In a source checkout the adapter also finds
the default `build/` library.

Use a context manager or `close()` for deterministic cleanup. `ParseError`
provides status, position, line, and column; `GrammarError` reports failures
during native construction. `ambiguous` and `conflicts` expose accepted-forest
ambiguity and unresolved parse-table conflict counts.

Each parser instance is reusable. Use separate instances for concurrent work;
an instance owns mutable parsing state.
