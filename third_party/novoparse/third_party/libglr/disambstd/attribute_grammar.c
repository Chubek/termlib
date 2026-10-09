#include "internal.h"
#include <stdlib.h>

typedef struct frame {
  const glr_forest_node_t *node;
  const struct frame *parent;
} frame_t;

static int evaluate (const glr_disambig_context_t *c,
    const glr_disambig_candidate_t *candidate, const glr_forest_node_t *node,
    const glr_disambstd_options_t *o, const frame_t *parent, double *value)
{
  if (!node) return -1;
  size_t depth = 0;
  for (const frame_t *p = parent; p; p = p->parent)
    if (p->node == node || ++depth > 1024) return -1;
  frame_t frame = {node, parent};
  double *children = calloc (node->child_count, sizeof (*children));
  if (node->child_count && (!children || !node->children))
    { free (children); return -1; }
  for (size_t i = 0; i < node->child_count; ++i)
    {
      int status = evaluate (c, candidate, node->children[i], o, &frame, &children[i]);
      if (status != 1) { free (children); return status; }
    }
  bool valid = o->attribute (c, node, candidate, children, node->child_count,
                             value, o->user_data);
  free (children);
  return valid ? 1 : 0;
}

GLR_STD_RUN(attribute_grammar)
{
  for (size_t i = 0; i < context->candidate_count; ++i)
    if (!context->candidates[i].rejected)
      {
        double value = 0;
        int status = evaluate (context, &context->candidates[i],
            context->candidates[i].node, options, NULL, &value);
        if (status < 0) return GLR_DISAMBIG_ERROR;
        if (!status) context->candidates[i].rejected = true;
        else context->candidates[i].score = value;
      }
  return glr_std_finish (context, winner);
}
