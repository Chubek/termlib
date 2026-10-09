/** ATN adaptive-lookahead engine: FIRST/FOLLOW analysis, window
    simulation, production prediction, conflict filtering, and the
    lookahead disambiguation hook.  See include/glr/atn.h. */
#include <glr/atn.h>
#include <glr/disambiguate.h>
#include <glr/parsetbl.h>
#include <glr/scannerless.h>

#include <stdlib.h>
#include <string.h>

struct glr_atn_follow
{
  size_t symbol_count;
  size_t width; /* symbol_count + 1; the extra column is end-of-input */
  size_t eof_column;
  bool *nullable; /* [symbol_count] */
  bool *first;    /* [symbol_count][width] */
  bool *follow;   /* [symbol_count][width] */
};

static bool
matrix_get (const bool *matrix, size_t width, size_t row, size_t column)
{
  return matrix[row * width + column];
}

static void
matrix_set (bool *matrix, size_t width, size_t row, size_t column)
{
  matrix[row * width + column] = true;
}

static bool
grammar_usable (const glr_grammar_t *grammar)
{
  if (grammar == NULL || grammar->symbol_count == 0
      || grammar->start_symbol == NULL)
    return false;
  for (size_t s = 0; s < grammar->symbol_count; ++s)
    {
      const glr_symbol_t *symbol = grammar->symbols[s];
      if (symbol == NULL || symbol->id != (int) s)
        return false;
    }
  for (size_t p = 0; p < grammar->production_count; ++p)
    {
      const glr_production_t *production = grammar->productions[p];
      if (production == NULL || production->head == NULL
          || production->id != (int) p)
        return false;
    }
  return true;
}

glr_atn_follow_t *
glr_atn_follow_compute (const glr_grammar_t *grammar)
{
  glr_atn_follow_t *follow;
  size_t n;
  size_t width;
  bool changed;
  if (!grammar_usable (grammar))
    return NULL;
  n = grammar->symbol_count;
  width = n + 1;
  if (width <= n || n > SIZE_MAX / width)
    return NULL;
  follow = calloc (1, sizeof (*follow));
  if (follow == NULL)
    return NULL;
  follow->symbol_count = n;
  follow->width = width;
  follow->eof_column = n;
  follow->nullable = calloc (n, sizeof (bool));
  follow->first = calloc (n * width, sizeof (bool));
  follow->follow = calloc (n * width, sizeof (bool));
  if (follow->nullable == NULL || follow->first == NULL
      || follow->follow == NULL)
    {
      glr_atn_follow_destroy (follow);
      return NULL;
    }
  for (size_t s = 0; s < n; ++s)
    if (glr_symbol_is_terminal (grammar->symbols[s]))
      matrix_set (follow->first, width, s, s);
  changed = true;
  while (changed)
    {
      changed = false;
      for (size_t p = 0; p < grammar->production_count; ++p)
        {
          const glr_production_t *production = grammar->productions[p];
          size_t head = (size_t) production->head->id;
          bool all_nullable = true;
          for (size_t b = 0; b < production->body_length; ++b)
            {
              size_t body = (size_t) production->body[b]->id;
              for (size_t c = 0; c < width; ++c)
                if (matrix_get (follow->first, width, body, c)
                    && !matrix_get (follow->first, width, head, c))
                  {
                    matrix_set (follow->first, width, head, c);
                    changed = true;
                  }
              if (!follow->nullable[body])
                {
                  all_nullable = false;
                  break;
                }
            }
          if (all_nullable && !follow->nullable[head])
            {
              follow->nullable[head] = true;
              changed = true;
            }
        }
    }
  matrix_set (follow->follow, width, (size_t) grammar->start_symbol->id,
              follow->eof_column);
  changed = true;
  while (changed)
    {
      changed = false;
      for (size_t p = 0; p < grammar->production_count; ++p)
        {
          const glr_production_t *production = grammar->productions[p];
          size_t head = (size_t) production->head->id;
          for (size_t i = 0; i < production->body_length; ++i)
            {
              size_t sym = (size_t) production->body[i]->id;
              bool rest_nullable = true;
              if (!glr_symbol_is_nonterminal (production->body[i]))
                continue;
              for (size_t j = i + 1; j < production->body_length; ++j)
                {
                  size_t rest = (size_t) production->body[j]->id;
                  for (size_t c = 0; c < width; ++c)
                    if (matrix_get (follow->first, width, rest, c)
                        && !matrix_get (follow->follow, width, sym, c))
                      {
                        matrix_set (follow->follow, width, sym, c);
                        changed = true;
                      }
                  if (!follow->nullable[rest])
                    {
                      rest_nullable = false;
                      break;
                    }
                }
              if (rest_nullable)
                for (size_t c = 0; c < width; ++c)
                  if (matrix_get (follow->follow, width, head, c)
                      && !matrix_get (follow->follow, width, sym, c))
                    {
                      matrix_set (follow->follow, width, sym, c);
                      changed = true;
                    }
            }
        }
    }
  return follow;
}

void
glr_atn_follow_destroy (glr_atn_follow_t *follow)
{
  if (follow == NULL)
    return;
  free (follow->nullable);
  free (follow->first);
  free (follow->follow);
  free (follow);
}

bool
glr_atn_first_contains (const glr_atn_follow_t *follow, int symbol_id,
                        int terminal_id)
{
  if (follow == NULL || symbol_id < 0 || terminal_id < 0
      || (size_t) symbol_id >= follow->symbol_count
      || (size_t) terminal_id >= follow->symbol_count)
    return false;
  return matrix_get (follow->first, follow->width, (size_t) symbol_id,
                     (size_t) terminal_id);
}

bool
glr_atn_follow_contains (const glr_atn_follow_t *follow, int nonterminal_id,
                         int terminal_or_eof)
{
  size_t column;
  if (follow == NULL || nonterminal_id < 0
      || (size_t) nonterminal_id >= follow->symbol_count)
    return false;
  if (terminal_or_eof == GLR_ATN_LOOKAHEAD_EOF)
    column = follow->eof_column;
  else if (terminal_or_eof >= 0
           && (size_t) terminal_or_eof < follow->symbol_count)
    column = (size_t) terminal_or_eof;
  else
    return false;
  return matrix_get (follow->follow, follow->width, (size_t) nonterminal_id,
                     column);
}

/* Validate one window token.  -1 is end-of-input and always legal;
   non-negative ids are legal for simulation (unknown ids simply die). */
static bool
window_token_legal (int token)
{
  return token == GLR_ATN_LOOKAHEAD_EOF || token >= 0;
}

int
glr_atn_is_prefix_viable (const glr_atn_t *atn, uint32_t start_state,
                          const int *symbols, size_t count)
{
  glr_atn_state_set_t *current = NULL;
  int viable = -1;
  if (atn == NULL || glr_atn_state (atn, start_state) == NULL
      || (count != 0 && symbols == NULL))
    return -1;
  for (size_t i = 0; i < count; ++i)
    if (!window_token_legal (symbols[i]))
      return -1;
  current = glr_atn_state_set_create ();
  if (current == NULL)
    return -1;
  if (glr_atn_state_set_add (current, start_state) != 0
      || glr_atn_epsilon_closure (atn, current) != 0)
    goto done;
  for (size_t i = 0; i < count; ++i)
    {
      glr_atn_state_set_t *next;
      if (symbols[i] == GLR_ATN_LOOKAHEAD_EOF)
        break;
      next = glr_atn_state_set_create ();
      if (next == NULL)
        goto done;
      if (glr_atn_step (atn, current, symbols[i], next) != 0)
        {
          glr_atn_state_set_destroy (next);
          goto done;
        }
      glr_atn_state_set_destroy (current);
      current = next;
      if (glr_atn_state_set_count (current) == 0)
        {
          viable = 0;
          goto done;
        }
    }
  viable = 1;
done:
  glr_atn_state_set_destroy (current);
  return viable;
}

long
glr_atn_viable_prefix_length (const glr_atn_t *atn, uint32_t start_state,
                              const int *symbols, size_t count)
{
  glr_atn_state_set_t *current = NULL;
  long consumed = -1;
  if (atn == NULL || glr_atn_state (atn, start_state) == NULL
      || (count != 0 && symbols == NULL))
    return -1;
  for (size_t i = 0; i < count; ++i)
    if (!window_token_legal (symbols[i]))
      return -1;
  current = glr_atn_state_set_create ();
  if (current == NULL)
    return -1;
  if (glr_atn_state_set_add (current, start_state) != 0
      || glr_atn_epsilon_closure (atn, current) != 0)
    goto done;
  consumed = 0;
  for (size_t i = 0; i < count; ++i)
    {
      glr_atn_state_set_t *next;
      if (symbols[i] == GLR_ATN_LOOKAHEAD_EOF)
        break;
      next = glr_atn_state_set_create ();
      if (next == NULL)
        {
          consumed = -1;
          goto done;
        }
      if (glr_atn_step (atn, current, symbols[i], next) != 0)
        {
          glr_atn_state_set_destroy (next);
          consumed = -1;
          goto done;
        }
      glr_atn_state_set_destroy (current);
      current = next;
      if (glr_atn_state_set_count (current) == 0)
        break;
      ++consumed;
    }
done:
  glr_atn_state_set_destroy (current);
  return consumed;
}

/* First outgoing SYMBOL edge of @p state labelled @p symbol_id, or NULL. */
static const glr_atn_transition_t *
find_symbol_edge (const glr_atn_t *atn, uint32_t state, int symbol_id)
{
  const glr_atn_state_t *s = glr_atn_state (atn, state);
  if (s == NULL)
    return NULL;
  for (size_t i = 0; i < s->transition_count; ++i)
    if (s->transitions[i].type == GLR_ATN_SYMBOL
        && s->transitions[i].symbol_id == symbol_id)
      return &s->transitions[i];
  return NULL;
}

/* Score one production's ATN path against the window.  Terminal RHS symbols
   must equal the window token; nonterminal RHS symbols consume one window
   token when that token is in the nonterminal's FIRST set.  Returns the
   number of window tokens matched, or -1 when the ATN has no entry for the
   production. */
static long
score_production (const glr_atn_t *atn, const glr_atn_follow_t *follow,
                  const glr_production_t *production, const int *lookahead,
                  size_t count)
{
  uint32_t state = glr_atn_production_start (atn, production->id);
  size_t i = 0;
  if (state == UINT32_MAX)
    return -1;
  for (size_t b = 0; b < production->body_length && i < count; ++b)
    {
      const glr_symbol_t *sym = production->body[b];
      const glr_atn_transition_t *edge;
      if (lookahead[i] == GLR_ATN_LOOKAHEAD_EOF)
        break;
      edge = find_symbol_edge (atn, state, sym->id);
      if (edge == NULL)
        break;
      if (glr_symbol_is_terminal ((glr_symbol_t *) sym))
        {
          if (sym->id != lookahead[i])
            break;
        }
      else if (!glr_atn_first_contains (follow, sym->id, lookahead[i]))
        break;
      state = edge->target;
      ++i;
    }
  return (long) i;
}

int
glr_atn_predict_production (const glr_atn_t *atn,
                            const glr_grammar_t *grammar, int nonterminal_id,
                            const int *lookahead, size_t lookahead_count,
                            int *out_production_id)
{
  glr_atn_follow_t *follow = NULL;
  const glr_symbol_t *head;
  long best = -1;
  int winner = -1;
  bool tie = false;
  bool any = false;
  if (atn == NULL || !grammar_usable (grammar) || out_production_id == NULL
      || nonterminal_id < 0
      || (size_t) nonterminal_id >= grammar->symbol_count
      || (lookahead_count != 0 && lookahead == NULL))
    return -1;
  head = grammar->symbols[nonterminal_id];
  if (!glr_symbol_is_nonterminal ((glr_symbol_t *) head))
    return -1;
  for (size_t i = 0; i < lookahead_count; ++i)
    if (!window_token_legal (lookahead[i]))
      return -1;
  follow = glr_atn_follow_compute (grammar);
  if (follow == NULL)
    return -1;
  for (size_t p = 0; p < grammar->production_count; ++p)
    {
      const glr_production_t *production = grammar->productions[p];
      long score;
      if (production->head->id != nonterminal_id)
        continue;
      any = true;
      score = score_production (atn, follow, production, lookahead,
                                lookahead_count);
      if (score < 0)
        continue;
      if (score > best)
        {
          best = score;
          winner = production->id;
          tie = false;
        }
      else if (score == best)
        tie = true;
    }
  glr_atn_follow_destroy (follow);
  if (!any || winner < 0 || tie)
    return 0;
  *out_production_id = winner;
  return 1;
}

/* Scan up to @p capacity terminal ids starting at @p start_pos (after
   skipping trivia).  Stops at end-of-input, on unscannable bytes, or on
   lexical ambiguity (more than one match: the window past this point is
   not deterministic).  Returns 0 on success, -1 on invalid input. */
int
glr_atn_scan_window (const glr_grammar_t *grammar, const char *input,
                     size_t input_length, const char *trivia, size_t start_pos,
                     int *window, size_t capacity, size_t *count_out,
                     bool *deterministic_out)
{
  size_t count = 0;
  bool deterministic = true;
  size_t trivia_length = trivia != NULL ? strlen (trivia) : 0;
  if (grammar == NULL || window == NULL || count_out == NULL
      || deterministic_out == NULL
      || (input_length != 0 && input == NULL))
    return -1;
  while (count < capacity)
    {
      glr_terminal_match_t *matches = NULL;
      size_t match_count = 0;
      size_t longest = 0;
      if (trivia != NULL)
        while (trivia_length <= input_length - start_pos
               && memcmp (input + start_pos, trivia, trivia_length) == 0)
          start_pos += trivia_length;
      if (start_pos >= input_length)
        break;
      if (glr_scannerless_scan (grammar, input, input_length, start_pos,
                                &matches, &match_count)
          != 0)
        return -1;
      if (match_count == 0)
        {
          free (matches);
          break;
        }
      if (match_count > 1)
        {
          /* The parser explores every alternative from here, so a
             deterministic window cannot extend past this point. */
          free (matches);
          deterministic = false;
          break;
        }
      longest = 0;
      for (size_t i = 1; i < match_count; ++i)
        if (matches[i].length > matches[longest].length)
          longest = i;
      window[count++] = matches[longest].symbol_id;
      start_pos = matches[longest].position + matches[longest].length;
      free (matches);
    }
  *count_out = count;
  *deterministic_out = deterministic;
  return 0;
}

/* Conservative conflict filter for one (state, lookahead) cell.
 *
 * - ACCEPT survives only on end-of-input.
 * - SHIFT always survives its own lookahead; with a deterministic deeper
 *   window it additionally needs an action in the shift target on the
 *   second token (bounded LR(2) viability under deterministic lexing).
 * - REDUCE survives only when the lookahead is in FOLLOW(head), which is
 *   exact: no accepting continuation can place anything else there.
 *
 * Unknown productions or an empty window keep the candidate rather than
 * risk killing a valid parse.  Returns 0 on success, -1 on invalid input.
 */
int
glr_atn_lookahead_filter (const glr_grammar_t *grammar,
                          const glr_atn_follow_t *follow,
                          const glr_parse_table_t *table, uint32_t state,
                          const glr_action_set_t *actions, const int *window,
                          size_t window_count, bool window_deterministic,
                          bool *keep)
{
  /* @p state identifies the conflict cell for diagnostics; viability is
     decided per action (shift targets carry their own next state). */
  (void) state;
  if (grammar == NULL || follow == NULL || table == NULL || actions == NULL
      || keep == NULL || (window_count != 0 && window == NULL))
    return -1;
  for (size_t a = 0; a < actions->action_count; ++a)
    {
      const glr_action_t *action = &actions->actions[a];
      int current = window_count > 0 ? window[0] : GLR_ATN_LOOKAHEAD_EOF;
      keep[a] = true;
      if (action->type == GLR_ACTION_ACCEPT)
        keep[a] = current == GLR_ATN_LOOKAHEAD_EOF;
      else if (action->type == GLR_ACTION_SHIFT)
        {
          const glr_action_set_t *next;
          uint32_t column;
          if (!window_deterministic || window_count < 2)
            continue;
          if (window[1] == GLR_ATN_LOOKAHEAD_EOF)
            column = glr_parse_table_eof_column (table);
          else if (window[1] >= 0)
            column = (uint32_t) window[1];
          else
            continue;
          next = glr_parse_table_get_actions (table, action->shift.next_state,
                                              column);
          if (next == NULL || next->action_count == 0)
            keep[a] = false;
        }
      else if (action->type == GLR_ACTION_REDUCE)
        {
          const glr_production_t *production = glr_grammar_get_production (
              grammar, (int) action->reduce.production_id);
          if (production == NULL || production->head == NULL)
            continue;
          keep[a] = glr_atn_follow_contains (follow, production->head->id,
                                             current);
        }
      else
        keep[a] = false;
    }
  return 0;
}

typedef struct
{
  const glr_grammar_t *grammar;
  glr_atn_t *atn;
  glr_atn_follow_t *follow;
  size_t max_depth;
} atn_hook_state_t;

static void
atn_hook_destroy (void *user_data)
{
  atn_hook_state_t *state = user_data;
  if (state == NULL)
    return;
  glr_atn_destroy (state->atn);
  glr_atn_follow_destroy (state->follow);
  free (state);
}

static glr_disambig_result_t
atn_hook_fn (glr_disambig_context_t *context, size_t *winner_index,
             void *user_data)
{
  atn_hook_state_t *state = user_data;
  if (state == NULL || context == NULL || winner_index == NULL)
    return GLR_DISAMBIG_NO_MATCH;
  for (size_t i = 0; i < context->candidate_count; ++i)
    {
      const glr_disambig_candidate_t *candidate = &context->candidates[i];
      if (candidate->rejected || candidate->production == NULL
          || candidate->production->head == NULL)
        continue;
      if (!glr_atn_follow_contains (state->follow,
                                    candidate->production->head->id,
                                    context->lookahead_symbol_id))
        glr_disambig_context_reject_candidate (context, i);
    }
  if (glr_disambig_context_active_count (context) == 1)
    {
      *winner_index = glr_disambig_context_last_active (context);
      return GLR_DISAMBIG_RESOLVED;
    }
  return GLR_DISAMBIG_NO_MATCH;
}

struct glr_disambig_hook *
glr_atn_lookahead_hook_create (const char *name, unsigned int priority,
                               const glr_grammar_t *grammar, size_t max_depth)
{
  atn_hook_state_t *state;
  struct glr_disambig_hook *hook;
  if (!grammar_usable (grammar))
    return NULL;
  if (max_depth == 0)
    max_depth = GLR_ATN_LOOKAHEAD_DEFAULT_DEPTH;
  if (max_depth > GLR_ATN_LOOKAHEAD_MAX_DEPTH)
    max_depth = GLR_ATN_LOOKAHEAD_MAX_DEPTH;
  state = calloc (1, sizeof (*state));
  if (state == NULL)
    return NULL;
  state->grammar = grammar;
  state->max_depth = max_depth;
  state->atn = glr_atn_from_grammar (grammar);
  state->follow = glr_atn_follow_compute (grammar);
  if (state->atn == NULL || state->follow == NULL)
    {
      atn_hook_destroy (state);
      return NULL;
    }
  hook = glr_disambig_hook_create (name != NULL ? name : "atn-lookahead",
                                   priority, atn_hook_fn, state,
                                   atn_hook_destroy);
  if (hook == NULL)
    atn_hook_destroy (state);
  return hook;
}
