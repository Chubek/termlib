# Parser modules

These are two different parsing models, not aliases. Both use the existing
native module ABI and return ordinary owned DOM values, accessible through
`std.json:get(tree, "children.0.name")`. Neither executes semantic-action code,
starts worker threads, launches a compiler, or downloads dependencies.

## `std.peg`

| Operation | Result |
|---|---|
| `compile(source)` | Owned compiled grammar handle; first declared rule is the start |
| `parse(grammar,input,steps?=1000000)` | Owned named-rule syntax tree; requires complete consumption |
| `test(grammar,input,steps?=1000000)` | Boolean full match; limit/allocation errors remain errors |
| `close(grammar)` | Releases compiled storage and invalidates aliases; idempotent |

Runnable example, also exercised across execution backends:

```termscript
use peg, json
const grammar = peg.compile("Root <- &Word Word !.; Word <- [a-z]+");
const tree = peg.parse(grammar, "hello");
peg.close(grammar);
const text = json.get(tree, "children.0.text");
G:puts(text);
```

### Grammar syntax and semantics

- `Name <- expression` defines a rule; references may be forward or recursive.
  Names are ASCII identifiers, at most 64 bytes. Duplicate and undefined names fail.
- Definitions end at `;`, EOF, or the next `Name <-`; whitespace and `#` line
  comments are ignored **in grammar source**, not implicitly in parsed input.
  Empty sequences/`''` are valid empty expressions. Input trivia must be explicit.
- `/` is ordered choice; juxtaposition is sequence. Choice only tries its next
  alternative if its own previous alternative fails. It does not reconsider a
  successful alternative because a later enclosing expression fails.
  Thus `S <- 'a' / 'ab'` does **not** fully match `ab`.
- Suffix `?`, `*`, `+` is optional, zero-or-more, one-or-more. Repetition is
  possessive, not regex-style backtracking over iteration counts.
- Prefix `&` and `!` are positive/negative lookahead. They consume no input and
  produce no captures. They bind less tightly than suffixes; `!A*` means `!(A*)`.
- Parentheses group expressions. `.` matches one byte, not one Unicode scalar.
- Single/double-quoted literals match exact bytes. Escapes are `\n`, `\r`, `\t`,
  `\\`, `\'`, `\"`, `\[`, `\]`, `\-`, `\^`, and `\xHH`. NUL is unsupported,
  matching the Termscript string model. Unsupported escapes fail explicitly.
- `[abc]`, `[a-z]`, `[^0-9]` are byte classes/ranges. Escape literal `]` inside
  a class. A trailing `-` is literal; descending ranges and empty classes fail.
  Unicode properties and POSIX named character classes are not implemented.
- No implicit lexer is used. No grammar actions, labels, imports, left-recursion
  extension or global grammar registry are provided. This is not `.ng` syntax.

Every successful **rule invocation** contributes a tree node with `name`, `text`,
zero-based byte `start`, exclusive byte `end`, and `children` in source order.
Literals/classes do not create nodes of their own. Failed alternatives and all
lookahead captures are discarded. Strings and children are independent copies;
the tree remains valid after another parse, `close`, or last-handle destruction.

The implementation deliberately rejects potentially nullable repetition and
direct/indirect left recursion, including through predicates/nullable prefixes.
This analysis is conservative and can reject unreachable recursive alternatives.
Use consuming right recursion instead. There is no packrat memoization or promise
of linear-time parsing: adversarial backtracking ends at the evaluation budget.

Limits: 64 KiB grammar source, 4096 expression nodes, 128 rules, 128 levels of
grammar nesting/expression height, 1 MiB input, 128 active rule invocations,
256 evaluation nesting levels, 65536 captures, and 1 MiB aggregate copied
rule-name/matched-text bytes in the result. The text budget does not count DOM
container/key overhead, which is separately bounded by capture/depth limits.
The optional step budget is 1–10000000; one expression evaluation costs a step.
Literal comparison and result copying have bounded byte costs, not per-byte steps.
`test` skips result capture/allocation and therefore does not incur result-tree
limits. Grammar/input syntax failures use `TS_ERR_PARSE`; exhausted limits use
`TS_ERR_LIMIT`, OOM uses `TS_ERR_NOMEM`. A false `test` result never swallows a
limit or allocation failure. A grammar remains usable after parse failure.

Diagnostics contain grammar byte offsets for compiler failures (including the
undefined reference's name) and furthest-probed input offsets for mismatches.
Successful prefix matches rejected for trailing input report the first trailing
byte, not a farther lookahead probe. Budgets are local to each parse and separate
from the VM's statement budget. Call inputs are borrowed; compilation source data
are not retained through borrowed pointers.

## `std.grammar`

This adapter uses the repository's pinned NovoParse native API and its bundled
libglr implementation. It builds scannerless context-free grammars without
inventing a second `.ng` reader or a command-line compiler invocation.

| Operation | Contract |
|---|---|
| `new(start_name)` | New owned mutable descriptor builder |
| `nonterminal(g,name)` | Declare a nonterminal |
| `literal(g,name,text)` | Declare an exact, nonempty byte terminal |
| `pattern(g,name,expression)` | Declare a POSIX ERE terminal; checked at build |
| `rule(g,head,body,label?)` | Append a production; body is a `std.list` of declared symbol names |
| `build(g,policy?="reject")` | Compile descriptor, permanently sealing mutation on success |
| `parse(g,input)` | Owned selected syntax tree, requiring complete consumption |
| `conflicts(g)` | Native table conflict count; requires successful build |
| `ambiguous(g)` | Ambiguity detected by the latest parse; requires a parse attempt |
| `close(g)` | Release descriptors/parser and invalidate aliases; idempotent |

Runnable builder example (verified by backend conformance):

```termscript
use grammar, list, json
const g = grammar.new("Root");
grammar.nonterminal(g, "Root");
grammar.pattern(g, "Word", "[a-z]+");
const body = list.new();
list.push(body, "Word");
grammar.rule(g, "Root", body, "Document");
grammar.build(g);
const tree = grammar.parse(g, "hello");
grammar.close(g);
const text = json.get(tree, "children.0.text");
G:puts(text);
```

Declare all referenced symbols before adding a rule. All nonterminals must have
a production before building. An empty body list represents an empty production;
empty terminals are rejected. Symbol names and optional labels are ASCII
identifiers of at most 64 bytes. Symbol/name/text/label data and body IDs are
copied when added. Failed additions do not partially publish entries.
Successful build seals the grammar; failed build leaves the descriptor usable
for further additions, but there is no replace/delete API. No trivia or comment
skipping is inserted in **input**; model those explicitly as terminals/rules.
Patterns are POSIX extended expressions, not PEGs, and cannot accept empty input.

`reject` is the default ambiguity policy. An ambiguous accepted input fails with
`TS_ERR_PARSE`; `ambiguous(g)` still reports true. `first` explicitly delegates
selection to NovoParse's deterministic first-derivation resolver. No ambiguity
is silently treated as an unambiguous tree. Results are **selected syntax trees,
not packed parse forests**; the adapter does not expose all derivations. Trees
have `name` (production label or symbol name), byte `start`/`end`, `children`, and
`text` (terminal spelling, otherwise nil). Trees own copies independent of parser
reuse/close. Native syntax failures preserve the runtime's offset and message.

Limits: 128 symbols, 256 productions, 32 symbols per body, 64 KiB aggregate
symbol/name/pattern/label storage, 4096 input bytes, 128 result-tree levels,
16384 result nodes and 1 MiB aggregate copied terminal/name bytes. Oversized
requests fail with `TS_ERR_LIMIT`, invalid builder operations with `TS_ERR_INVAL`,
native build/input/ambiguity failures with `TS_ERR_PARSE`, OOM with `TS_ERR_NOMEM`.

**Security limitation:** grammar authors must be trusted. The pinned GLR engine
does not expose hard internal allocation, parse-table-size or execution budgets.
The descriptor/input/output caps are not a denial-of-service sandbox or proof of
bounded GLR execution time. Use OS-level isolation for untrusted GLR grammars.
No cross-thread cancellation is provided. This limitation is not hidden behind
the VM's step budget. Use the budgeted PEG engine when its semantics suffice.

## Ownership, threads and coverage

Both handles release their resources after partial initialization and on final
destruction. Explicit close affects every alias; cleanup functions never close
borrowed caller descriptors or launch native processes. Serialize shared-handle
access against close/mutation; separate handles may be used in separate threads.
No new external dependency or guessed third-party API was introduced.

Tests cover backend conformance, ambiguity policies, empty/truncated/malformed
input, invalid terminal patterns, ordered choice, rollback, predicates, forward
references, recursion and nullable-cycle rejection, nested input, step/capture/
compile limits, alias/close/result ownership, recovery, and a malformed-source
corpus. The quoted shell PEG from `examples/tsh.tsc` is tested independently;
this does **not** mean the illustrative shell application runs. The ELF fault
harness checks adapter/runtime allocation cleanup, not allocations inside the
NovoParse shared dependency. Hardware, network services and host terminfo/font
installations are not used by these parser tests.
