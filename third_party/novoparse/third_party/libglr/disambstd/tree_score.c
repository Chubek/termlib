#include "internal.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
  const glr_forest_node_t *node;
  double value;
  bool complete;
} entry_t;
typedef struct {
  entry_t *entries;
  size_t count, capacity;
  glr_disambig_score_fn fn;
  void *data;
  bool probability;
} memo_t;

static bool score (memo_t *m, const glr_disambig_context_t *c,
    const glr_disambig_candidate_t *a, const glr_forest_node_t *node,
    size_t depth, double *out)
{
  *out = 0;
  if (!node) return true;
  if (depth > 1024 || (node->child_count && !node->children)) return false;
  for (size_t i = 0; i < m->count; ++i)
    if (m->entries[i].node == node)
      {
        *out = m->entries[i].value;
        return m->entries[i].complete; /* An unfinished entry is a cycle. */
      }
  if (m->count == m->capacity)
    {
      size_t capacity = m->capacity ? m->capacity * 2 : 32;
      entry_t *entries = realloc (m->entries, capacity * sizeof (*entries));
      if (!entries) return false;
      m->entries = entries; m->capacity = capacity;
    }
  size_t index = m->count++;
  m->entries[index] = (entry_t){node, 0, false};
  double value = m->fn ? m->fn (c, node, a, m->data) : (m->probability ? 1 : 0);
  if (!isfinite (value) || (m->probability && (value < 0 || value > 1))) return false;
  if (m->probability) value = value == 0 ? -INFINITY : log (value);
  for (size_t i = 0; i < node->child_count; ++i)
    {
      double child;
      if (!score (m, c, a, node->children[i], depth + 1, &child)) return false;
      value += child;
    }
  if (isnan (value) || (!m->probability && !isfinite (value))) return false;
  m->entries[index].value = value; m->entries[index].complete = true;
  *out = value;
  return true;
}

glr_disambig_result_t glr_std_tree_choose (glr_disambig_context_t *c,
    size_t *winner, glr_disambig_score_fn fn, void *data, bool probability)
{
  if (winner) *winner = SIZE_MAX;
  if (!c || (c->candidate_count && !c->candidates)) return GLR_DISAMBIG_ERROR;
  double *scores = calloc (c->candidate_count, sizeof (*scores));
  if (!scores && c->candidate_count) return GLR_DISAMBIG_ERROR;
  double best = 0; bool found = false;
  for (size_t i = 0; i < c->candidate_count; ++i)
    if (!c->candidates[i].rejected)
      {
        const glr_disambig_candidate_t *a = &c->candidates[i];
        memo_t memo = {NULL, 0, 0, fn, data, probability};
        double value;
        bool valid = score (&memo, c, a, a->node, 0, &value);
        free (memo.entries);
        double base = probability ? a->probability : a->score;
        if (!valid || !isfinite (base) || (probability && (base < 0 || base > 1)))
          { free (scores); return GLR_DISAMBIG_ERROR; }
        if (probability) base = base == 0 ? -INFINITY : log (base);
        scores[i] = value + base;
        if (isnan (scores[i]) || (!probability && !isfinite (scores[i])))
          { free (scores); return GLR_DISAMBIG_ERROR; }
        if (!found || (probability ? scores[i] > best : scores[i] < best)) best = scores[i];
        found = true;
      }
  for (size_t i = 0; i < c->candidate_count; ++i)
    if (!c->candidates[i].rejected && scores[i] != best) c->candidates[i].rejected = true;
  free (scores);
  return glr_std_finish (c, winner);
}
