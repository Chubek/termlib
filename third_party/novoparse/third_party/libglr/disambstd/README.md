# Standard disambiguation tools

`glrdisambstd` implements all twenty strategies in this directory. Include
`<glr/disambstd.h>` and create them through one interface:

```c
glr_disambstd_options_t options;
glr_disambstd_options_init(&options);
options.priority = 100;
glr_disambig_hook_t *hook =
    glr_disambstd_create_named("longest-match", &options);
if (!hook) {
    /* Invalid configuration or allocation failure. */
} else if (glr_parser_add_disambiguator(parser, hook) != 0) {
    glr_disambig_hook_destroy(hook);
}
```

The enum-based `glr_disambstd_create()` has the same options. Enumerate names
with `glr_disambstd_name()` and `GLR_DISAMBSTD_COUNT`. Existing six
`glr_disambig_*_hook_create()` functions remain available.

## Runtime strategies

| Factory name | Configuration and behavior |
| --- | --- |
| `precedence` | Highest `rank`, or candidate `precedence` by default. |
| `associativity` | Highest precedence, then leftmost/rightmost grouping according to `associativity` and `split_position`. Left associativity prefers the largest split. Equal splits remain tied; nonassociative conflicts return an error. |
| `predicate` | Required `predicate` accepts syntactically valid candidates. |
| `semantic` | Required `predicate` checks frontend semantic constraints. |
| `dynamic-programming` | Minimize candidate `score` plus the sum of per-node `score` callback values. Shared subtrees are memoized per candidate; repeated child occurrences still contribute separately. |
| `probability` | Maximize candidate `probability` times per-node callback probabilities. Log-space computation avoids product underflow. Values must be finite and in [0,1]; initialize the candidate probability to **1** for a neutral prior. Zero means impossible, not an unspecified prior. |
| `attribute-grammar` | Required bottom-up `attribute` callback synthesizes a numeric attribute from ordered child attributes. False rejects a derivation; the root attribute becomes candidate `score`. Follow with a cost hook to rank it if desired. |
| `case-sensitivity` | Required `text` callback and `expected_text`; exact, case-sensitive byte comparison. |
| `indentation-and-layout` | Required `layout` callback supplies concrete column/line relations as integer differences. |
| `island-parsing` | Required `rank` supplies recognized island coverage or negative water/recovery cost; highest rank survives. |
| `layout-sensitive` | Required `layout` callback supplies symbolic integer-difference constraints; candidates with unsatisfiable layouts are rejected. |
| `lexical` | Optional admissibility `predicate`, then lexical `rank` (defaults to candidate precedence). |
| `longest-match` | Keep maximum-length spans with a common start. Inverted spans or different starts are errors. |
| `name-resolution` | Required `predicate` resolves identifiers using the frontend's scope/symbol environment. |
| `post-parse-filtering` | Required whole-tree `predicate`, applied to completed derivation candidates. |
| `prefer-avoid` | Highest `rank` or candidate precedence; positive prefers, zero is neutral, negative avoids. An all-avoid set retains the least disfavored alternatives. |
| `scannerless-priorities` | Optional follow/reject `predicate`, then production `rank` or candidate precedence. |

Callbacks receive the context, candidate, and configured `user_data`. Language
rules, symbol tables, island classification, and production restrictions belong
to the frontend and are supplied through these callbacks. The library does not
interpret production annotation strings or assume a particular grammar syntax.

Hooks skip rejected candidates. Ranking ties retain all best candidates so later
hooks can decide. Filtering to zero candidates is an error. A successful factory
copies the options and takes responsibility for calling `destroy(user_data)`;
failed creation leaves ownership with the caller. Referenced strings, reports,
and external environments must outlive the hook. Callbacks should be deterministic
during one invocation. Use separate state/reports when invoking concurrently.

Tree callbacks operate on ordered derivation trees/DAGs. A frontend whose packed
nodes represent alternatives must expose each alternative as a candidate rather
than presenting alternatives as simultaneous children. Cycles are errors for
attribute and scoring walks; both reject depths over 1024.

The parser runs hooks at ambiguity choice points, not as an unconditional
validation pass. For completed trees or a non-libglr parser, populate a
`glr_disambig_context_t` and call `hook->fn(&context, &winner, hook->user_data)`.
Destroy the hook when finished. This makes the same strategies usable by an
elkhound frontend without requiring a `glr_parser_t`.

## Layout constraints

`glr_disambstd_layout_t` is a conjunction of `x - y <= upper` constraints.
Variable 0 is a relative origin. Pin column `x` to 4 with `(x,0,4)` and
`(0,x,-4)`. Require `child > parent` with `(parent,child,-1)`. Equality uses two
opposite inequalities. A callback returning false rejects its candidate.
An empty conjunction is satisfiable. Invalid indices, missing arrays, or numeric
bounds outside the supported safe int64 range produce an error.

The callback owns its returned array and must keep it valid until the next
callback invocation. Satie's IDL solver checks the conjunction with Bellman-Ford;
general nonlinear or linear arithmetic beyond integer differences is not exposed.

## Bounded grammar analysis

`bounded-sat`, `bounded-smt`, and `counter-example-guided` also have a standalone
entry point suitable for grammar compilers:

```c
glr_disambstd_options_t options;
glr_disambstd_options_init(&options);
options.max_tokens = 6;
options.max_depth = 10;
glr_disambstd_report_t report = {0};
glr_disambstd_check_result_t status = glr_disambstd_check(
    GLR_DISAMBSTD_BOUNDED_SAT, grammar, &options, &report);
if (status == GLR_DISAMBSTD_CHECK_AMBIGUOUS) {
    /* report.tokens is the sentence (terminal symbol IDs).
       report.productions[0] and [1] are distinct preorder production-ID
       sequences describing the two ordered trees, including epsilon rules. */
}
glr_disambstd_report_clear(&report);
```

Initialize reports to zero before first use. Checks replace the previous report;
`glr_disambstd_report_clear()` frees its owned arrays and is idempotent. Callback
derivation views are borrowed only for the duration of the callback.

The checker enumerates leftmost derivations within **both** `max_tokens` and
`max_depth`, groups equal terminal yields, and encodes two distinct one-hot
derivation choices into SAT. Groups are checked in increasing token length.
Epsilon productions, unit recursion, and arbitrary production arity are supported.
Satie CDCL selects a pair. SMT additionally checks the conjunction of both
derivations' IDL constraints; infeasible pairs are blocked and SAT is retried.
Variable indices must identify the same sentence layout across derivations.
An SMT witness also supplies `report.layout_values`, normalized so variable 0
is zero, as a concrete layout satisfying both derivations.
The `derivation_filter` callback supplies restrictions and optional layout
constraints; SAT ignores its arithmetic constraints, whereas SMT uses them.

Counterexample-guided mode calls `refine(first, second, user_data)` on a witness.
Return true after updating the state used by `derivation_filter`; all derivations
are then re-evaluated. Return false to stop and report that witness. This mode
uses IDL too. Refinements restrict the enumerated derivations; do not mutate the
grammar during a check. Non-progress terminates at `max_iterations` with `LIMIT`.

Results distinguish `AMBIGUOUS`, `CLEAR` **within the selected token/depth bounds**,
`LIMIT`, and `ERROR`. `max_derivations` budgets partial and completed enumeration
states, not just accepted trees. At most 2048 alternatives per yield are encoded;
larger groups return `LIMIT`. Maximum accepted depth is 256 and maximum state
budget is 1,000,000. Enumeration is exponential and intended for short
counterexamples. This is a finite derivation encoding, not the compact incremental
encoding from the reference paper; solver learning is not preserved between
checks. It cannot prove general CFG unambiguity.

When installed as parser hooks, these three tools run the same analysis using
`context.grammar` and write to `options.report`, if supplied. They do not select
or reject runtime candidates: a completed analysis returns `NO_MATCH`, and
`ERROR`/`LIMIT` returns a hook error. Prefer the standalone API for grammar-time
analysis to avoid repeating it at every runtime ambiguity.

## Build and downstream use

`BUILD_DISAMBSTD=ON` builds all strategies. The public interface is C11-compatible;
the solver adapter requires C++20. It uses the existing `third_party/satie`
CDCL/IDL headers, Satie memory-resource source, and its vendored memtkx headers.
No additional library download or full Satie frontend/plugin build is required.

```cmake
project(my_frontend LANGUAGES C CXX)
find_package(libglr CONFIG REQUIRED)
add_executable(my_frontend frontend.c)
target_link_libraries(my_frontend PRIVATE libglr::glrdisambstd)
```

The exported CMake target supplies libglr and the static library's link
dependencies. When linking manually, use a C++ linker and link
`glrdisambstd`, `glr`, `sfsexp`, `m`, and pthreads (plus libglr's cache dependencies
if enabled). The optional `BUILD_DISAMBSTD=OFF` configuration keeps the core
library C-only.
