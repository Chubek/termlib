# ANSI-C: C11 grammar, AST, and ATN demonstration

This is a **design example for the future Moosedog frontend**. Moosedog is
not implemented, so the `.grm` file is a specification, not an executable
generator input today. The directory name is historical; the syntax target
is **ISO C11**, using [N1570](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf),
principally sections 6.4–6.9 and Annex A. It includes C99 features retained
by C11 and old-style function definitions with explicit declaration specifiers.
It does not introduce implicit-int function definitions or C23 extensions.

## Files

| File | Purpose |
| --- | --- |
| `ANSI-C.grm` | Lexer specification, C11 productions, AST schema/actions, proposed ATN configuration |
| `Support.gpp` | Actual GPP prelude for specification metadata and configuration |
| `ANSICParser.c` | Runnable libglr demonstration: small C-shaped grammar, ATN, adaptive parsing, forest-to-AST projection |
| `README.md` | Input contract, action semantics, runtime mapping, and build instructions |

## Input and lexical contract

The full grammar consumes **preprocessed C tokens**, after translation phases
1–4. The future input adapter must perform trigraph replacement, line splicing,
comment replacement, directive processing, includes, conditional compilation,
and macro expansion, retaining a map to original source locations. `#include`
and `#define` are not syntactic productions here. GPP preprocessing of the
**specification** is a separate operation from preprocessing **C source**.

The lexer must preserve preprocessing-token boundaries:

- Match the longest complete token; keywords win equal-length identifier ties.
  `iffy` is one identifier, not `if` plus `fy`.
- Validate the entire preprocessing number. Invalid spellings such as `09`,
  `1lul`, or `0x1.2` must not be split into smaller valid tokens.
- Integer patterns cover decimal, octal, hexadecimal, and valid `u/U`, `l/L`,
  `ll/LL` suffix combinations. Floating patterns cover decimal exponents and
  hexadecimal floats with mandatory `p/P` exponents.
- Character/string tokens retain encoding prefixes, escapes, and spelling.
  Decode escapes and concatenate adjacent strings in later phases, checking
  encoding compatibility. `StringFragments` also accepts unjoined adjacent
  literal tokens. A conforming adapter may provide an already joined token.
- Map `<:`, `:>`, `<%`, `%>` to bracket/brace token kinds after preprocessing.
  `%:` and `%:%:` belong to directive/macro processing. Retain original spelling
  in source annotations when normalizing a digraph.
- Validate universal character names and non-ASCII identifier characters against
  C11's constraints and Annex D. The regex recognizes their shape, not every
  semantic restriction on code points. Reject embedded NUL and invalid UTF-8.

`FallbackLexer = "Scannerless"` is a proposed frontend choice. It still needs
the same token boundaries and priorities. Raw libglr scannerless matching alone
does not implement C preprocessing or these contextual lexical rules.

### Typedef names and scope

`TYPEDEF_NAME` is a **contextual** token, supplied by the proposed
`VisibleTypedefName` classifier. It has no standalone regex. An arbitrary
`IDENTIFIER` is no longer accepted as a type specifier.

The classifier/resolver must track ordinary identifiers and typedefs per scope,
including function parameters, block scopes and declaration-form `for` loops.
Bindings take effect at the appropriate point of declaration: for example,
`typedef int T; void f(void) { int T = 1; T++; }` uses the inner object `T`
in its initializer and subsequent expressions. An outer typedef spelling can
also name a tag, member or label in a distinct namespace. `Name` permits either
identifier token kind in these naming positions and in declarators; primary
expressions only accept ordinary identifiers.

Classification cannot be a global mutable table shared by speculative GLR
branches. Keep branch-local scope state or resolve name-dependent alternatives
over the forest. Reclassify lookahead at scope/binding boundaries; do not cache
`TYPEDEF_NAME` decisions across them. Enumerator bindings are ordinary identifiers
and need the same point-of-declaration treatment.

## Syntax coverage

The grammar includes:

- All C11 storage classes, function specifiers, type specifiers and qualifiers,
  including `_Thread_local`, `_Noreturn`, `_Atomic`, and `_Alignas`.
- Structures/unions, anonymous members, bit-fields, flexible array syntax,
  enums with a single optional trailing comma, and member-level static assertions.
- Named and abstract declarators, nested pointers, function pointers, prototypes,
  variadic parameters, identifier-list functions and old-style declaration lists.
- Array bounds, qualifiers, both placements of `static`, and prototype-scope
  `[*]`. Abstract declarators follow their separate C11 syntax: `[const *]`
  belongs to a named declarator, not a direct abstract declarator.
- Scalar/braced initializers, nested member/index designators and compound literals.
- Every expression precedence level, right-associative assignment/conditional
  expressions, `sizeof`, `_Alignof`, `_Generic`, postfix operations and calls.
- Labels, selection/iteration/jump statements, mixed block declarations/statements,
  empty expression statements, and all optional `for` clauses.
- Translation units, declarations, function definitions and `_Static_assert`.

An empty translation unit, empty C11 initializer braces, repeated enum trailing
commas, and a label immediately before `}` have no productions. This distinguishes
standard syntax from common implementation extensions.

The syntax grammar is not a type checker. A later semantic pass enforces valid
specifier combinations, declaration constraints, lvalues, constant expressions,
prototype-only VLA forms, generic association uniqueness/compatibility, jump
targets, and return types. The dangling `else` belongs to the nearest unmatched
`if`; the full frontend must select that reading before building one AST.

## AST schema and deferred actions

`CNode` is a tagged union, not an empty placeholder. Its variants are:

| Variant | Payload |
| --- | --- |
| `CToken` | Owned spelling for identifiers, literals, specifiers and operators |
| `CUnit` | Ordered external declarations |
| `CDeclaration` | Form tag and ordered children for declarations, functions, parameters, members, assertions |
| `CType` | Form tag and children for record/enum types, typedef references, atomic/alignment/type-name forms |
| `CDeclarator` | Form tag and children preserving pointer/array/function binding and grouping |
| `CExpression` | Form tag and operands; operators/encodings remain distinguishable |
| `CStatement` | Form tag and statement-specific children |
| `CInitializer` | Scalar/braced/designated values and member/index designators |
| `CSequence` | Ordered items for declaration, parameter, argument and initializer lists |

Every alternative has an action. Precedence-only forwarding rules return the
child, punctuation disappears, and recursive lists are flattened. Distinct tags
preserve prefix/postfix increment, type/expression `sizeof`, named/anonymous
records, and each `for` form. Child order is explicit in each action; for example:

```text
Tree(CDeclaration, function, specifiers, declarator, body)
Tree(CExpression, assignment, lhs, operator-token, rhs)
Tree(CStatement, if-else, condition, then-statement, else-statement)
Tree(CInitializer, designated, designators, value)
```

### Proposed action-helper contract

These are declarative action expressions for the future Moosedog action compiler,
**not C function names or existing libglr APIs**. Hyphenated form labels are symbolic
strings, not subtraction expressions. The helper semantics are fully specified:

| Expression | Meaning in terms of the AST schema |
| --- | --- |
| `Token($n)` | Copy the RHS token spelling into a `CToken` leaf and wrap it as `CNode` |
| `Tree(Family, form, children...)` | Construct the named family with a copied form string and an ordered list of child `CNode`s, then wrap it as `CNode` |
| `Sequence(child)` | Construct a `CSequence` containing one child |
| `Append(sequence, child)` | Return a sequence with all prior items followed by the new child, without changing a shared input sequence |
| `Unit(sequence)` | Construct a `CUnit` from an external-declaration sequence's items |
| `return $n;` | Forward the child unchanged; introduce no AST wrapper |

For example, `Tree(CExpression, add, $1, $3)` is shorthand for the JSON example's
constructor style:

```text
MakeVariant(CExpression,
    MakeNode(CExpression, "add", MakeList($1, $3)))
```

`Tree` with no children uses an empty list. Tokens are wrapped explicitly rather
than accidentally being passed as node pointers. Constructors receive reduction
spans via `ASTAnnotations`; token leaves retain exact source spans and source-map
identity. AST construction runs **after** syntactic/name-resolution policies select
a derivation. It must not execute while adaptive lookahead speculates. Generated
ASTs own their strings/children (or an owning arena); forests and input buffers can
then be released independently. Listener/visitor emission is enabled.

## ATN pipeline

The `ATN` block is proposed Moosedog configuration. Its intended lowering is:

```text
final grammar -> LR table + glr_atn_from_grammar()
input adapter -> tokens -> GLR conflict handling + adaptive lookahead
    -> accepted packed forest -> C-specific resolution -> deferred AST actions
```

The generated parser would map `AdaptiveLookahead` and `MaxDepth` to
`glr_parser_set_adaptive_lookahead()` and
`glr_parser_set_adaptive_lookahead_depth()`. Enable prediction only if **both**
`Config.UseATN` and `ATN.AdaptiveLookahead` are true. `--no-atn` is the master
disable; standalone GPP's `ANSI_C_NO_ATN` sets both flags false.

`PreserveAmbiguity` means unresolved alternatives remain in the GLR forest.
`EmitStatistics` exposes `glr_parser_get_adaptive_stats()`. Build ATNs after any
grammar transformation so production/symbol ids agree. The runnable driver uses
parser-owned ATNs and destroys parsers before their borrowed grammars.

Current libglr filtering is conservative and bounded: the parser's filter uses
FOLLOW membership and a deterministic second-token shift-continuation check;
larger configured windows do not imply arbitrary recursive C prediction.
`glr_atn_predict_production()` is a prefix-scoring inspection helper, not a C
semantic oracle. Neither that helper nor ATNs resolve typedef shadowing, `_Generic`
type selection, or dangling else. The demo prints prediction results for inspection
and does not use them to discard parse alternatives.

## Preprocess the specification

From this directory, with GPP installed:

```sh
gpp -C --include Support.gpp ANSI-C.grm -o /tmp/ANSI-C.expanded.grm
gpp -C -DANSI_C_NO_ATN -DANSI_C_DEBUG \
    --include Support.gpp ANSI-C.grm -o /tmp/ANSI-C.plain.grm
gpp -C -DANSI_C_LOOKAHEAD_DEPTH=4 \
    --include Support.gpp ANSI-C.grm -o /tmp/ANSI-C.depth4.grm
```

The `-C` option is significant: modes set inside an included file are restored
on return. It keeps quotes and regex escapes intact in the main grammar input.
The support prelude has an include guard, namespaced macros, a depth range check,
and no cpp-only `#`/`##` stringification or invalid version-string token pasting.
GPP leaves action strings alone; it does not implement the AST action compiler.
There is no `moosedog` invocation or generated-header build to run yet.

## Build and run the C demonstration

From the repository root, after building libglr:

```sh
cc -std=c11 -Wall -Wextra -Werror -Iinclude \
    moosedog/examples/ANSI-C/ANSICParser.c \
    build/libglr.a build/libsfsexp.a -pthread -o /tmp/ansic_demo
/tmp/ansic_demo
```

This directly links the core static-library objects used by the demo. Consumers
using an installed library can instead use `pkg-config --cflags --libs --static libglr`.

The driver exercises a deliberately small grammar with declarations, assignments,
and nested `if` statements. It reports ATN entries, prediction results and adaptive
statistics. For unambiguous input it builds and serializes an owned `glr_ast_t`:

```text
AST: (initialized-declaration int a 3)
AST: (expression-statement (assignment a (integer 3)))
```

For nested `if`/`else`, it reports that AST construction is deferred. It never
chooses the first packed alternative arbitrarily. Parsing the same ambiguous
sentence with adaptive lookahead disabled also succeeds. Zero pruned actions is
a legitimate result: these ambiguities cannot be eliminated by looking ahead.

The driver is a runtime illustration, **not a full ANSI-C parser generated from
the specification**. Its explicit production-to-AST mapping is separate from
the full `.grm` schema.

## Verification

The example was checked by:

- Compiling/running `ANSICParser.c` with C11 and warnings treated as errors.
- Running GPP with default, disabled-ATN, debug and depth-override settings,
  including invalid-depth rejection and preservation of regex escapes.
- Checking all rule/token references, AST family names and `$n` action positions.
- Extracting productions into an independent Earley recognizer for representative
  positive/negative C11 syntax and lexical cases, including abstract declarators,
  VLA forms, compound literals, generic selections and enum trailing commas.

These checks validate the demonstration's structure and representative syntax;
they do not constitute end-to-end Moosedog generation or C11 semantic conformance.
