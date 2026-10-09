#ifndef GLR_ATN_H
#define GLR_ATN_H

/**
 * @file atn.h
 * @brief Augmented Transition Network (ATN) support for GLR front ends.
 *
 * An ATN is a compact epsilon-NFA representation of a grammar's production
 * alternatives.  libglr uses it as a generator/runtime intermediate: a rule
 * entry has epsilon edges to the alternatives for that rule and every
 * production is represented by a linear path of symbol transitions.
 *
 * This deliberately keeps the ATN independent of the LR parse table.  A
 * front end can therefore use it for production matching, lookahead,
 * diagnostics, or code generation and still hand the same grammar to the
 * ordinary GLR parser.
 */

#include <glr/grammar.h>
#include <glr/parsetbl.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct glr_atn glr_atn_t;
typedef struct glr_atn_state_set glr_atn_state_set_t;

typedef enum
{
  GLR_ATN_EPSILON = 0,
  GLR_ATN_SYMBOL = 1
} glr_atn_transition_type_t;

typedef struct
{
  glr_atn_transition_type_t type;
  uint32_t target;
  int symbol_id;
} glr_atn_transition_t;

typedef struct
{
  glr_atn_transition_t *transitions;
  size_t transition_count;
  size_t transition_capacity;
  bool accepting;
  int production_id;
} glr_atn_state_t;

/** Create an empty ATN with state zero as its start state. */
glr_atn_t *glr_atn_create(void);

/** Destroy an ATN. NULL is accepted. */
void glr_atn_destroy(glr_atn_t *atn);

/** Add a state and return its numeric id, or UINT32_MAX on failure. */
uint32_t glr_atn_add_state(glr_atn_t *atn);

/** Add an epsilon transition. */
int glr_atn_add_epsilon(glr_atn_t *atn, uint32_t from, uint32_t to);

/** Add a transition consuming one grammar symbol. */
int glr_atn_add_symbol(glr_atn_t *atn, uint32_t from, uint32_t to,
                       int symbol_id);

/** Mark a state as accepting for a production (production_id may be -1). */
int glr_atn_set_accepting(glr_atn_t *atn, uint32_t state, bool accepting,
                          int production_id);

/** Return the ATN's start state, or UINT32_MAX for NULL. */
uint32_t glr_atn_start_state(const glr_atn_t *atn);

/** Return the number of states. */
size_t glr_atn_state_count(const glr_atn_t *atn);

/** Return a state by id, or NULL. */
const glr_atn_state_t *glr_atn_state(const glr_atn_t *atn, uint32_t state);

/** Return a non-owning set of ATN state ids. */
glr_atn_state_set_t *glr_atn_state_set_create(void);
void glr_atn_state_set_destroy(glr_atn_state_set_t *set);
void glr_atn_state_set_clear(glr_atn_state_set_t *set);
int glr_atn_state_set_add(glr_atn_state_set_t *set, uint32_t state);
bool glr_atn_state_set_contains(const glr_atn_state_set_t *set, uint32_t state);
size_t glr_atn_state_set_count(const glr_atn_state_set_t *set);
uint32_t glr_atn_state_set_at(const glr_atn_state_set_t *set, size_t index);

/**
 * Compute epsilon closure in-place.  Repeated epsilon edges are handled and
 * duplicate states are suppressed.
 */
int glr_atn_epsilon_closure(const glr_atn_t *atn, glr_atn_state_set_t *set);

/**
 * Consume one symbol from a state set and compute the epsilon closure of the
 * resulting states.  The output may alias neither implementation storage nor
 * the input set; aliasing the two public set objects is supported.
 */
int glr_atn_step(const glr_atn_t *atn, const glr_atn_state_set_t *current,
                 int symbol_id, glr_atn_state_set_t *next);

/** True if the set contains at least one accepting state. */
bool glr_atn_state_set_accepting(const glr_atn_t *atn,
                                  const glr_atn_state_set_t *set);

/**
 * Match a complete sequence of grammar symbol ids against the ATN.
 * Returns 1 for a match, 0 for rejection, and -1 on invalid input/memory
 * failure.  This is an NFA/ATN operation; it does not perform CFG recursion.
 */
int glr_atn_match(const glr_atn_t *atn, const int *symbols, size_t count);

/** Match starting at an explicit ATN state (useful for a grammar rule entry). */
int glr_atn_match_from(const glr_atn_t *atn, uint32_t start_state,
                       const int *symbols, size_t count);

/**
 * Compile grammar productions into an ATN.  Each nonterminal gets a rule
 * entry state; that state epsilon-branches to the production paths belonging
 * to the nonterminal.  Production paths are independently accepting, which
 * makes the result useful for GLR reduction/production matching.
 */
glr_atn_t *glr_atn_from_grammar(const glr_grammar_t *grammar);

/** Return the rule entry state for a nonterminal symbol, or UINT32_MAX. */
uint32_t glr_atn_rule_start(const glr_atn_t *atn, int nonterminal_id);

/** Return the production entry state for a production id, or UINT32_MAX.
 *
 * The entry state is the first state of that production's linear path: the
 * epsilon-successor of the head nonterminal's rule entry that begins this
 * production.  Simulating from here (instead of the shared rule entry)
 * attributes lookahead consumption to one specific alternative, which is
 * what adaptive prediction needs.  Returns UINT32_MAX for NULL ATNs,
 * negative ids, or ids the ATN was not compiled with.
 */
uint32_t glr_atn_production_start(const glr_atn_t *atn, int production_id);

/* ========================================================================
 * Adaptive lookahead support
 *
 * The routines below turn the ATN from a passive production matcher into
 * the lookahead engine of the parser pipeline.  The model is deliberately
 * bounded and conservative:
 *
 * - A lookahead window is a short sequence of grammar terminal ids
 *   followed by the parser's deterministic forward scan.  The value -1
 *   denotes end-of-input and is never passed to glr_atn_step(); the
 *   prediction helpers interpret it directly.
 * - FIRST/FOLLOW sets are computed over the grammar with the standard
 *   fixpoint.  The extra end-of-input column lives at index
 *   grammar->symbol_count, matching the parse table's EOF column
 *   convention (see parsetbl.h).
 * - Prediction never invents derivations: when the window does not
 *   distinguish the alternatives, the helpers report "undecided" and the
 *   parser falls back to full GLR forking plus user disambiguators.
 * ======================================================================== */

/** Default lookahead window depth (current token plus following tokens). */
#define GLR_ATN_LOOKAHEAD_DEFAULT_DEPTH 8u
/** Hard upper bound accepted by glr_parser_set_adaptive_lookahead_depth(). */
#define GLR_ATN_LOOKAHEAD_MAX_DEPTH 64u
/** Lookahead window sentinel for end-of-input (never a real symbol id). */
#define GLR_ATN_LOOKAHEAD_EOF (-1)

/** Nullable/FIRST/FOLLOW summary computed from a grammar. */
typedef struct glr_atn_follow glr_atn_follow_t;

/** Compute nullable, FIRST, and FOLLOW sets.  NULL on invalid input. */
glr_atn_follow_t *glr_atn_follow_compute(const glr_grammar_t *grammar);

/** Destroy a lookahead summary.  NULL is accepted. */
void glr_atn_follow_destroy(glr_atn_follow_t *follow);

/** True when terminal @p terminal_id is in FIRST(@p symbol_id).
 *
 * Nonterminals contribute their fixpoint FIRST set; terminals only contain
 * themselves.  Returns false on any invalid input.
 */
bool glr_atn_first_contains(const glr_atn_follow_t *follow, int symbol_id,
                            int terminal_id);

/** True when @p terminal_or_eof can follow @p nonterminal_id.
 *
 * @p terminal_or_eof is a terminal symbol id, or GLR_ATN_LOOKAHEAD_EOF for
 * end-of-input.  Returns false on any invalid input (NULL summary,
 * non-nonterminal id, unknown terminal).
 */
bool glr_atn_follow_contains(const glr_atn_follow_t *follow, int nonterminal_id,
                             int terminal_or_eof);

/**
 * Test whether @p symbols is a viable prefix from @p start_state.
 *
 * Returns 1 when the whole sequence can be consumed (epsilon closure
 * applied before and after every step), 0 when simulation dies early, and
 * -1 on invalid input.  GLR_ATN_LOOKAHEAD_EOF terminates the viable span:
 * a window ending at end-of-input is viable exactly when its terminal
 * prefix is.  This is a pure NFA reachability query; nonterminal-labelled
 * edges are only traversed for the exact symbol id given, never expanded.
 */
int glr_atn_is_prefix_viable(const glr_atn_t *atn, uint32_t start_state,
                             const int *symbols, size_t count);

/**
 * Length of the longest viable prefix of @p symbols from @p start_state.
 *
 * Returns the count of leading symbols consumable before simulation dies
 * (possibly zero, possibly all of @p count), or -1 on invalid input.
 * GLR_ATN_LOOKAHEAD_EOF stops the scan and is not counted.
 */
long glr_atn_viable_prefix_length(const glr_atn_t *atn, uint32_t start_state,
                                  const int *symbols, size_t count);

/**
 * Predict which alternative of @p nonterminal_id matches @p lookahead.
 *
 * Every production headed by the nonterminal is scored by simulating its
 * ATN path (see glr_atn_production_start()) against the window: terminal
 * RHS symbols must equal the window token, nonterminal RHS symbols consume
 * one window token when that token is in the nonterminal's FIRST set.
 * The unique longest-prefix winner is stored to @p out_production_id.
 *
 * Returns 1 when decided, 0 when undecided (tie, empty alternative set, or
 * empty window against several empty-matching alternatives), and -1 on
 * invalid input (NULL ATN/grammar/output, unknown nonterminal, or a window
 * holding an out-of-range symbol id).
 */
int glr_atn_predict_production(const glr_atn_t *atn,
                               const glr_grammar_t *grammar,
                               int nonterminal_id, const int *lookahead,
                               size_t lookahead_count, int *out_production_id);

/* Forward declaration: the hook type lives in <glr/disambiguate.h>.  The
 * factory is declared here so the ATN lookahead pipeline has a single
 * home; include both headers to use it. */
struct glr_disambig_hook;

/**
 * Build a sound lookahead disambiguation hook for a grammar.
 *
 * The hook rejects REDUCE candidates whose head nonterminal cannot be
 * followed by the conflict's lookahead symbol (FOLLOW check) and reports a
 * winner when exactly one candidate survives.  Otherwise it returns
 * GLR_DISAMBIG_NO_MATCH so later hooks still run.  SHIFT/ACCEPT candidates
 * are never rejected.  The hook owns a private ATN compiled from
 * @p grammar plus its FOLLOW summary; @p max_depth bounds the window the
 * hook inspects (currently the conflict lookahead itself) and is clamped
 * to GLR_ATN_LOOKAHEAD_MAX_DEPTH.  Returns NULL on invalid input.
 *
 * Ownership of the returned hook follows the disambiguation API: a
 * successful glr_parser_add_disambiguator() call transfers ownership to
 * the parser, otherwise the caller must destroy it.
 */
struct glr_disambig_hook *glr_atn_lookahead_hook_create(
    const char *name, unsigned int priority, const glr_grammar_t *grammar,
    size_t max_depth);

/* ========================================================================
 * Parser-pipeline helpers (used by parser.c for automatic filtering)
 * ======================================================================== */

/**
 * Scan up to @p capacity terminal ids starting at @p start_pos.
 *
 * Trivia is skipped with the same literal rule the parser uses.  Scanning
 * stops at end-of-input, on unscannable bytes, or on lexical ambiguity
 * (more than one match at one offset, which the GLR engine explores as
 * parallel alternatives, so no deterministic window extends past it).
 * @p deterministic_out reports whether the end of the window was reached
 * without ambiguity.  End-of-input is reported by a short count, never by
 * a sentinel element.  Returns 0 on success, -1 on invalid input.
 */
int glr_atn_scan_window(const glr_grammar_t *grammar, const char *input,
                        size_t input_length, const char *trivia,
                        size_t start_pos, int *window, size_t capacity,
                        size_t *count_out, bool *deterministic_out);

/**
 * Conservative conflict filter for one (state, lookahead) action cell.
 *
 * @p window[0] is the conflict's own lookahead (GLR_ATN_LOOKAHEAD_EOF for
 * end-of-input); further elements are deterministically scanned tokens and
 * are only consulted when @p window_deterministic is true.  ACCEPT survives
 * on end-of-input only, SHIFT additionally needs next-state viability on
 * the second token when a deeper deterministic window exists, and REDUCE
 * survives exactly when the lookahead is in FOLLOW(head).  Unknown
 * productions keep their candidate.  @p keep must hold
 * actions->action_count entries.  Returns 0 on success, -1 on invalid
 * input.
 */
int glr_atn_lookahead_filter(const glr_grammar_t *grammar,
                             const glr_atn_follow_t *follow,
                             const glr_parse_table_t *table, uint32_t state,
                             const glr_action_set_t *actions,
                             const int *window, size_t window_count,
                             bool window_deterministic, bool *keep);

#ifdef __cplusplus
}
#endif

#endif /* GLR_ATN_H */
