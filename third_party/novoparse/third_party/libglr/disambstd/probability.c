#include "internal.h"
#include <stdlib.h>

typedef struct {
  glr_disambig_score_fn fn;
  glr_disambig_destroy_fn destroy;
  void *data;
} state_t;

static void destroy (void *data)
{
  state_t *s = data;
  if (s->destroy) s->destroy (s->data);
  free (s);
}
static glr_disambig_result_t run (glr_disambig_context_t *c, size_t *w, void *data)
{
  state_t *s = data;
  return glr_std_tree_choose (c, w, s->fn, s->data, true);
}
glr_disambig_hook_t *glr_disambig_probability_hook_create (
    const char *name, unsigned int priority, glr_disambig_score_fn fn,
    void *data, glr_disambig_destroy_fn destructor)
{
  state_t *s = malloc (sizeof (*s));
  if (!s) return NULL;
  *s = (state_t){fn, destructor, data};
  glr_disambig_hook_t *hook = glr_disambig_hook_create (
      name ? name : "probability", priority, run, s, destroy);
  if (!hook) free (s);
  return hook;
}
