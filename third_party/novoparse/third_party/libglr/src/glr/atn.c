#include <glr/atn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct glr_atn
{
  glr_atn_state_t *states;
  size_t state_count;
  size_t state_capacity;
  uint32_t start;
  uint32_t *rule_starts;
  size_t rule_start_count;
  uint32_t *prod_starts;
  size_t prod_start_count;
};

struct glr_atn_state_set
{
  uint32_t *states;
  size_t count;
  size_t capacity;
};

static int grow(void **ptr, size_t *capacity, size_t needed, size_t element_size)
{
  size_t cap = *capacity;
  void *p;
  if (needed <= cap)
    return 0;
  if (cap == 0)
    cap = 8;
  while (cap < needed)
    {
      if (cap > SIZE_MAX / 2)
        {
          cap = needed;
          break;
        }
      cap *= 2;
    }
  if (cap > SIZE_MAX / element_size)
    return -1;
  p = realloc(*ptr, cap * element_size);
  if (p == NULL)
    return -1;
  *ptr = p;
  *capacity = cap;
  return 0;
}

glr_atn_t *
glr_atn_create(void)
{
  glr_atn_t *atn = calloc(1, sizeof(*atn));
  if (atn == NULL)
    return NULL;
  atn->start = glr_atn_add_state(atn);
  if (atn->start == UINT32_MAX)
    {
      free(atn);
      return NULL;
    }
  return atn;
}

void
glr_atn_destroy(glr_atn_t *atn)
{
  if (atn == NULL)
    return;
  for (size_t i = 0; i < atn->state_count; ++i)
    free(atn->states[i].transitions);
  free(atn->states);
  free(atn->rule_starts);
  free(atn->prod_starts);
  free(atn);
}

uint32_t
glr_atn_add_state(glr_atn_t *atn)
{
  glr_atn_state_t *state;
  uint32_t id;
  if (atn == NULL || atn->state_count >= UINT32_MAX)
    return UINT32_MAX;
  if (grow((void **)&atn->states, &atn->state_capacity,
           atn->state_count + 1, sizeof(*atn->states)) != 0)
    return UINT32_MAX;
  id = (uint32_t)atn->state_count++;
  state = &atn->states[id];
  memset(state, 0, sizeof(*state));
  state->production_id = -1;
  return id;
}

static int
add_transition(glr_atn_t *atn, uint32_t from, uint32_t to,
               glr_atn_transition_type_t type, int symbol_id)
{
  glr_atn_state_t *state;
  glr_atn_transition_t *transition;
  if (atn == NULL || from >= atn->state_count || to >= atn->state_count)
    return -1;
  state = &atn->states[from];
  if (grow((void **)&state->transitions, &state->transition_capacity,
           state->transition_count + 1, sizeof(*state->transitions)) != 0)
    return -1;
  transition = &state->transitions[state->transition_count++];
  transition->type = type;
  transition->target = to;
  transition->symbol_id = symbol_id;
  return 0;
}

int
glr_atn_add_epsilon(glr_atn_t *atn, uint32_t from, uint32_t to)
{
  return add_transition(atn, from, to, GLR_ATN_EPSILON, -1);
}

int
glr_atn_add_symbol(glr_atn_t *atn, uint32_t from, uint32_t to, int symbol_id)
{
  if (symbol_id < 0)
    return -1;
  return add_transition(atn, from, to, GLR_ATN_SYMBOL, symbol_id);
}

int
glr_atn_set_accepting(glr_atn_t *atn, uint32_t state, bool accepting,
                       int production_id)
{
  if (atn == NULL || state >= atn->state_count)
    return -1;
  atn->states[state].accepting = accepting;
  atn->states[state].production_id = accepting ? production_id : -1;
  return 0;
}

uint32_t
glr_atn_start_state(const glr_atn_t *atn)
{
  return atn == NULL ? UINT32_MAX : atn->start;
}

size_t
glr_atn_state_count(const glr_atn_t *atn)
{
  return atn == NULL ? 0 : atn->state_count;
}

const glr_atn_state_t *
glr_atn_state(const glr_atn_t *atn, uint32_t state)
{
  if (atn == NULL || state >= atn->state_count)
    return NULL;
  return &atn->states[state];
}

glr_atn_state_set_t *
glr_atn_state_set_create(void)
{
  return calloc(1, sizeof(glr_atn_state_set_t));
}

void
glr_atn_state_set_destroy(glr_atn_state_set_t *set)
{
  if (set == NULL)
    return;
  free(set->states);
  free(set);
}

void
glr_atn_state_set_clear(glr_atn_state_set_t *set)
{
  if (set != NULL)
    set->count = 0;
}

bool
glr_atn_state_set_contains(const glr_atn_state_set_t *set, uint32_t state)
{
  if (set == NULL)
    return false;
  for (size_t i = 0; i < set->count; ++i)
    if (set->states[i] == state)
      return true;
  return false;
}

int
glr_atn_state_set_add(glr_atn_state_set_t *set, uint32_t state)
{
  if (set == NULL)
    return -1;
  if (glr_atn_state_set_contains(set, state))
    return 0;
  if (grow((void **)&set->states, &set->capacity, set->count + 1,
           sizeof(*set->states)) != 0)
    return -1;
  set->states[set->count++] = state;
  return 0;
}

size_t
glr_atn_state_set_count(const glr_atn_state_set_t *set)
{
  return set == NULL ? 0 : set->count;
}

uint32_t
glr_atn_state_set_at(const glr_atn_state_set_t *set, size_t index)
{
  if (set == NULL || index >= set->count)
    return UINT32_MAX;
  return set->states[index];
}

int
glr_atn_epsilon_closure(const glr_atn_t *atn, glr_atn_state_set_t *set)
{
  size_t cursor = 0;
  if (atn == NULL || set == NULL)
    return -1;
  while (cursor < set->count)
    {
      uint32_t id = set->states[cursor++];
      const glr_atn_state_t *state = glr_atn_state(atn, id);
      if (state == NULL)
        return -1;
      for (size_t i = 0; i < state->transition_count; ++i)
        if (state->transitions[i].type == GLR_ATN_EPSILON
            && glr_atn_state_set_add(set, state->transitions[i].target) != 0)
          return -1;
    }
  return 0;
}

int
glr_atn_step(const glr_atn_t *atn, const glr_atn_state_set_t *current,
              int symbol_id, glr_atn_state_set_t *next)
{
  glr_atn_state_set_t *source_copy = NULL;
  const glr_atn_state_set_t *source = current;
  if (atn == NULL || current == NULL || next == NULL || symbol_id < 0)
    return -1;

  if (current == next)
    {
      source_copy = glr_atn_state_set_create();
      if (source_copy == NULL)
        return -1;
      for (size_t i = 0; i < current->count; ++i)
        if (glr_atn_state_set_add(source_copy, current->states[i]) != 0)
          {
            glr_atn_state_set_destroy(source_copy);
            return -1;
          }
      source = source_copy;
    }

  glr_atn_state_set_clear(next);
  for (size_t i = 0; i < source->count; ++i)
    {
      const glr_atn_state_t *state = glr_atn_state(atn, source->states[i]);
      if (state == NULL)
        {
          glr_atn_state_set_destroy(source_copy);
          return -1;
        }
      for (size_t j = 0; j < state->transition_count; ++j)
        {
          const glr_atn_transition_t *t = &state->transitions[j];
          if (t->type == GLR_ATN_SYMBOL && t->symbol_id == symbol_id
              && glr_atn_state_set_add(next, t->target) != 0)
            {
              glr_atn_state_set_destroy(source_copy);
              return -1;
            }
        }
    }
  {
    int result = glr_atn_epsilon_closure(atn, next);
    glr_atn_state_set_destroy(source_copy);
    return result;
  }
}

bool
glr_atn_state_set_accepting(const glr_atn_t *atn,
                            const glr_atn_state_set_t *set)
{
  if (atn == NULL || set == NULL)
    return false;
  for (size_t i = 0; i < set->count; ++i)
    {
      const glr_atn_state_t *state = glr_atn_state(atn, set->states[i]);
      if (state != NULL && state->accepting)
        return true;
    }
  return false;
}

int
glr_atn_match_from(const glr_atn_t *atn, uint32_t start_state,
                   const int *symbols, size_t count)
{
  glr_atn_state_set_t *current;
  glr_atn_state_set_t *next;
  int result = -1;
  if (atn == NULL || start_state >= atn->state_count
      || (count != 0 && symbols == NULL))
    return -1;
  current = glr_atn_state_set_create();
  next = glr_atn_state_set_create();
  if (current == NULL || next == NULL)
    goto done;
  if (glr_atn_state_set_add(current, start_state) != 0
      || glr_atn_epsilon_closure(atn, current) != 0)
    goto done;
  for (size_t i = 0; i < count; ++i)
    {
      if (glr_atn_step(atn, current, symbols[i], next) != 0)
        goto done;
      {
        glr_atn_state_set_t *tmp = current;
        current = next;
        next = tmp;
      }
    }
  result = glr_atn_state_set_accepting(atn, current) ? 1 : 0;
done:
  glr_atn_state_set_destroy(current);
  glr_atn_state_set_destroy(next);
  return result;
}

int
glr_atn_match(const glr_atn_t *atn, const int *symbols, size_t count)
{
  return glr_atn_match_from(atn, glr_atn_start_state(atn), symbols, count);
}

static bool
atn_grammar_valid(const glr_grammar_t *g)
{
  if (g == NULL || g->start_symbol == NULL
      || !glr_symbol_is_nonterminal(g->start_symbol))
    return false;
  for (size_t p = 0; p < g->production_count; ++p)
    {
      const glr_production_t *prod = g->productions[p];
      if (prod == NULL || prod->head == NULL
          || !glr_symbol_is_nonterminal(prod->head))
        return false;
      for (size_t b = 0; b < prod->body_length; ++b)
        {
          bool owned = false;
          for (size_t s = 0; s < g->symbol_count; ++s)
            if (g->symbols[s] == prod->body[b])
              {
                owned = true;
                break;
              }
          if (!owned)
            return false;
        }
    }
  return true;
}

glr_atn_t *
glr_atn_from_grammar(const glr_grammar_t *grammar)
{
  const glr_grammar_t *g = grammar;
  glr_atn_t *atn;
  if (!atn_grammar_valid(g))
    return NULL;
  atn = glr_atn_create();
  if (atn == NULL)
    return NULL;
  atn->rule_start_count = g->symbol_count;
  if (g->symbol_count > SIZE_MAX / sizeof(*atn->rule_starts))
    goto fail;
  atn->rule_starts = malloc(g->symbol_count * sizeof(*atn->rule_starts));
  if (g->symbol_count != 0 && atn->rule_starts == NULL)
    goto fail;
  for (size_t i = 0; i < g->symbol_count; ++i)
    atn->rule_starts[i] = UINT32_MAX;
  atn->prod_start_count = g->production_count;
  if (g->production_count > SIZE_MAX / sizeof(*atn->prod_starts))
    goto fail;
  atn->prod_starts = malloc(g->production_count * sizeof(*atn->prod_starts));
  if (g->production_count != 0 && atn->prod_starts == NULL)
    goto fail;
  for (size_t i = 0; i < g->production_count; ++i)
    atn->prod_starts[i] = UINT32_MAX;

  for (size_t s = 0; s < g->symbol_count; ++s)
    {
      const glr_symbol_t *symbol = g->symbols[s];
      uint32_t rule_start;
      if (symbol == NULL || !glr_symbol_is_nonterminal((glr_symbol_t *)symbol))
        continue;
      rule_start = glr_atn_add_state(atn);
      if (rule_start == UINT32_MAX)
        goto fail;
      atn->rule_starts[s] = rule_start;
      if (glr_atn_add_epsilon(atn, atn->start, rule_start) != 0)
        goto fail;
      for (size_t p = 0; p < g->production_count; ++p)
        {
          const glr_production_t *prod = g->productions[p];
          uint32_t from;
          if (prod->head->id != (int)s)
            continue;
          from = glr_atn_add_state(atn);
          if (from == UINT32_MAX || glr_atn_add_epsilon(atn, rule_start, from) != 0)
            goto fail;
          if (prod->id >= 0
              && (size_t)prod->id < atn->prod_start_count
              && atn->prod_starts[prod->id] == UINT32_MAX)
            atn->prod_starts[prod->id] = from;
          for (size_t b = 0; b < prod->body_length; ++b)
            {
              uint32_t to = glr_atn_add_state(atn);
              if (to == UINT32_MAX
                  || glr_atn_add_symbol(atn, from, to, prod->body[b]->id) != 0)
                goto fail;
              from = to;
            }
          if (glr_atn_set_accepting(atn, from, true, prod->id) != 0)
            goto fail;
        }
    }
  return atn;
fail:
  glr_atn_destroy(atn);
  return NULL;
}

uint32_t
glr_atn_rule_start(const glr_atn_t *atn, int nonterminal_id)
{
  if (atn == NULL || nonterminal_id < 0
      || (size_t)nonterminal_id >= atn->rule_start_count)
    return UINT32_MAX;
  return atn->rule_starts[nonterminal_id];
}

uint32_t
glr_atn_production_start(const glr_atn_t *atn, int production_id)
{
  if (atn == NULL || production_id < 0
      || (size_t)production_id >= atn->prod_start_count)
    return UINT32_MAX;
  return atn->prod_starts[production_id];
}
