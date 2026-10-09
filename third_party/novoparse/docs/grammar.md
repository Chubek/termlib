# Grammar reference

## Rules and expressions

Every directive and rule ends with `;`. Names use ASCII identifiers
`[A-Za-z_][A-Za-z0-9_]*`. The `__np_` prefix is reserved for generated symbols.
Whitespace and `#`, `//`, and `/* ... */` comments are ignored outside literals
and regular expressions.

```text
document: item ("," item)* "."? => Document;
item: NAME | "(" document ")";
empty: %empty => Empty;
```

- `:` introduces a rule; `|` separates alternatives.
- Adjacent symbols form a sequence.
- Parentheses group expressions and alternatives.
- `?`, `*`, and `+` mean optional, zero-or-more, and one-or-more.
- An empty alternative, or `%empty`, matches empty input.
- `=> Label` names the syntax-tree node for that alternative. The default is
  the rule's name. Labels follow identifier syntax.

An alternative may end with `%prec TOKEN` or `%prec "literal"` before its node
label. Rule names must be unique; declare all alternatives in the same rule.
Forward references are supported. Repetition of a nullable expression is
rejected because it admits infinitely many empty derivations.

Quoted literals use single or double quotes. Supported escapes are `\n`, `\r`,
`\t`, `\\`, `\"`, and `\'`. Terminal literals must be nonempty. Grammar strings
cannot contain NUL bytes.

## Directives

| Directive | Meaning |
| --- | --- |
| `%grammar Name;` | Generated parser/module name; defaults to `Parser`. |
| `%start rule;` | Start rule; defaults to the first rule. |
| `%token NAME "literal";` | A named literal terminal. |
| `%token NAME /expression/;` | A named POSIX ERE terminal. |
| `%skip /expression/;` | Optional trivia matched between terminals and at the input boundaries. |
| `%skip "literal";` | Skip a run of one or more copies of a literal. |
| `%left terminals...;` | Declare left-associative precedence. |
| `%right terminals...;` | Declare right-associative precedence. |
| `%nonassoc terminals...;` | Reject chained operators at that precedence. |
| `%disambiguate reject;` | Require a unique accepted derivation; the default. |
| `%disambiguate first;` | Deterministically select an accepted derivation. |
| `%rewrite name;` | Apply a named `libglr` grammar rewrite before table construction. |

Each precedence line has a higher level than the preceding line. Terminals can
be token names or literals. A production inherits the precedence of its
rightmost terminal, unless overridden by `%prec`. Precedence resolves
shift/reduce conflicts; other alternatives continue through GLR parsing.

```text
%left "+" "-";
%left "*" "/";
%right "^";

expression: expression "+" expression => Add
          | expression "*" expression => Multiply
          | expression "^" expression => Power
          | "-" expression %prec "^" => Negate
          | "n" => Name;
```

`first` selects the earliest production in the runtime grammar, breaking
same-production split ties by the longest earlier child. It operates on
accepted forests, so failed lexical branches do not cause a viable reading to
be discarded. `parser.ambiguous` still reports whether the accepted forest
contained alternatives.

## Patterns and trivia

Regular expressions use POSIX extended syntax, not PCRE or Python syntax.
Escape a delimiter slash with `\/`. `\t`, `\r`, and `\n` in patterns are
translated to their corresponding characters; other escapes reach POSIX regex
unchanged. Invalid and empty-matching patterns are rejected by the native
runtime when constructing a parser.

The scanner explores overlapping terminals and all nonempty matching prefix
lengths. For `A /ab|a/` and `S: A "b";`, both `ab` and `abb` are accepted: the
grammar determines which length of `A` is viable.

Use a skip pattern that matches a complete trivia region, such as
`/[ \t\r\n]+/`. Trivia is represented internally by helper productions and is
omitted from syntax trees. EBNF helpers are flattened into their parent node.
Other grammar-normalization helpers remain visible.

## Rewriters

Rewrite directives are applied in declaration order using a generated GRL
S-expression program. Supported names:

```text
remove-epsilon-productions           remove-unit-productions
eliminate-useless-symbols            remove-left-recursion
left-factor                         make-lr-compatible
eliminate-ambiguity                  remove-duplicate-productions
remove-self-unit-productions         eliminate-unreachable-symbols
eliminate-unproductive-symbols       eliminate-unused-terminals
augment-start-symbol                isolate-terminals
left-binarize                       right-binarize
chomsky-normal-form                 right-factor
remove-right-recursion              reverse-productions
inline-single-production-nonterminals
merge-equivalent-nonterminals
```

For example, `%rewrite remove-duplicate-productions;` removes structurally
identical, unlabeled productions. Labeled/precedence-bearing productions retain
metadata, and cleanup passes preserve metadata on surviving productions.
New productions introduced by structural transformations use their head name
as their node label and have no inherited precedence. A rewrite can therefore
change tree shape as well as grammar structure.

`eliminate-ambiguity` verifies the resulting table and fails if conflicts
remain; normalization is not a general ambiguity proof.

## Diagnostics

The frontend reports errors as `file:line:column: message`. `check` validates
grammar syntax, names, references, precedence, IR structure, and start-symbol
productivity. Native construction additionally validates patterns, executes
rewrites, and checks parse-table construction.

Runtime syntax and ambiguity errors carry a zero-based byte offset and
one-based line/byte-column. Syntax trees use zero-based, end-exclusive byte
spans over the original input. Terminal lexemes include their exact source
bytes; named tokens retain their names, while anonymous terminals receive
stable `__np_literal_N` names.

## Limits

Hostile or accidental inputs are bounded so the frontend fails with a
diagnostic instead of exhausting memory or the C stack:

- Grammar/IR/profile files are capped at 8 MiB.
- S-expression nesting is capped at 256 levels and 1,000,000 nodes.
- Grammar EBNF nesting is capped at 256 levels.
- Grammars are capped at 65,536 symbols, 262,144 productions, 256 symbols
  per production body, and 1024 precedence levels.
- Duplicate `%rewrite` names are rejected.
- Empty `()` groups are rejected; write `%empty` or an explicit empty
  alternative instead.
