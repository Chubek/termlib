#include "peg/peg.h"
#include "common/ts_std_common.h"
#include <stdlib.h>
#include <string.h>

enum { GRAMMAR_LIMIT = 65536, NODE_LIMIT = 4096, RULE_LIMIT = 128,
       DEPTH_LIMIT = 128, CAPTURE_LIMIT = 65536, OUTPUT_LIMIT = 1048576,
       DEFAULT_STEPS = 1000000, MAX_STEPS = 10000000 };
typedef enum { EMPTY, LITERAL, CLASS, ANY, REF, SEQUENCE, CHOICE,
               OPTIONAL, STAR, PLUS, AND, NOT } kind_t;
typedef struct
{
  kind_t kind;
  int left, right, rule;
  unsigned height;
  size_t offset;
  char *text;
  unsigned char bits[32];
  bool nullable;
} node_t;
typedef struct { char name[65]; int body; bool nullable; unsigned visited; size_t offset; } rule_t;
typedef struct
{
  node_t nodes[NODE_LIMIT];
  rule_t rules[RULE_LIMIT];
  size_t node_count, rule_count;
  bool closed;
} peg_t;
typedef struct
{
  peg_t *grammar;
  const char *source;
  size_t position, length;
  unsigned depth;
  TS_Status status;
  TS_Error *error;
} reader_t;
static const char tag;

static TS_Status
failure (TS_Error *e, TS_Status status, size_t offset, const char *message)
{ ts_error_set (e, status, 0, offset, "%s", message); return status; }
static void release (peg_t *g)
{
  for (size_t i = 0; i < g->node_count; i++)
    { free (g->nodes[i].text); g->nodes[i].text = NULL; }
  g->closed = true;
}
static void destroy (void *p) { release (p); free (p); }
static int error_at (reader_t *p, TS_Status status, const char *message)
{
  if (!p->status) p->status = failure (p->error, status, p->position, message);
  return -1;
}
static void skip (reader_t *p)
{
  while (p->position < p->length)
    {
      char c = p->source[p->position];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->position++;
      else if (c == '#')
        { while (p->position < p->length && p->source[p->position] != '\n') p->position++; }
      else break;
    }
}
static bool first_name (unsigned char c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool next_name (unsigned char c)
{ return first_name (c) || (c >= '0' && c <= '9'); }
static bool identifier (reader_t *p, char name[65])
{
  skip (p);
  size_t start = p->position;
  if (!first_name ((unsigned char)p->source[start])) return false;
  while (next_name ((unsigned char)p->source[p->position])) p->position++;
  size_t len = p->position - start;
  if (len > 64) { error_at (p, TS_ERR_LIMIT, "PEG rule name exceeds 64 bytes"); return false; }
  memcpy (name, p->source + start, len); name[len] = 0; return true;
}
static int new_node (reader_t *p, kind_t kind, int left, int right)
{
  peg_t *g = p->grammar;
  if (g->node_count == NODE_LIMIT) return error_at (p, TS_ERR_LIMIT, "PEG exceeds 4096 expression nodes");
  unsigned height = 1;
  if (left >= 0) height = g->nodes[left].height + 1;
  if (right >= 0 && g->nodes[right].height + 1 > height) height = g->nodes[right].height + 1;
  if (height > DEPTH_LIMIT) return error_at (p, TS_ERR_LIMIT, "PEG expression nesting exceeds 128");
  int id = (int)g->node_count++;
  g->nodes[id] = (node_t){ .kind = kind, .left = left, .right = right, .height = height,
                          .rule = -1, .offset = p->position };
  return id;
}
static int byte (reader_t *p)
{
  if (p->position == p->length) return error_at (p, TS_ERR_PARSE, "unterminated PEG literal or class");
  unsigned char c = (unsigned char)p->source[p->position++];
  if (c != '\\') return c;
  if (p->position == p->length) return error_at (p, TS_ERR_PARSE, "unterminated PEG escape");
  c = (unsigned char)p->source[p->position++];
  switch (c)
    {
    case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t';
    case '\\': case '\'': case '"': case '[': case ']': case '-': case '^': return c;
    case 'x':
      {
        unsigned value = 0;
        for (unsigned i = 0; i < 2; i++)
          {
            if (p->position == p->length) return error_at (p, TS_ERR_PARSE, "PEG hex escape needs two digits");
            c = (unsigned char)p->source[p->position++];
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16) return error_at (p, TS_ERR_PARSE, "invalid PEG hex escape");
            value = value * 16 + digit;
          }
        if (!value) return error_at (p, TS_ERR_UNSUPPORTED, "PEG strings cannot contain NUL");
        return (int)value;
      }
    default: return error_at (p, TS_ERR_PARSE, "unsupported PEG escape");
    }
}
static int expression (reader_t *p);
static int primary (reader_t *p)
{
  skip (p);
  size_t offset = p->position;
  char c = p->source[p->position];
  if (c == '(')
    {
      p->position++;
      if (++p->depth > DEPTH_LIMIT) return error_at (p, TS_ERR_LIMIT, "PEG group nesting exceeds 128");
      int result = expression (p);
      p->depth--;
      skip (p);
      if (result < 0 || p->source[p->position] != ')') return error_at (p, TS_ERR_PARSE, "expected ')' in PEG");
      p->position++; return result;
    }
  if (c == '\'' || c == '"')
    {
      p->position++;
      ts_sbuf_t text; ts_sbuf_init (&text);
      while (p->position < p->length && p->source[p->position] != c)
        {
          int ch = byte (p);
          if (ch < 0) { ts_sbuf_free (&text); return -1; }
          if (ts_sbuf_ch (&text, (char)ch))
            { ts_sbuf_free (&text); return error_at (p, TS_ERR_NOMEM, "out of memory"); }
        }
      if (p->position == p->length)
        { ts_sbuf_free (&text); return error_at (p, TS_ERR_PARSE, "unterminated PEG literal"); }
      p->position++;
      int id = new_node (p, LITERAL, -1, -1);
      if (id < 0) { ts_sbuf_free (&text); return -1; }
      p->grammar->nodes[id].text = ts_sbuf_take (&text);
      if (!p->grammar->nodes[id].text) return error_at (p, TS_ERR_NOMEM, "out of memory");
      return id;
    }
  if (c == '[')
    {
      p->position++;
      int id = new_node (p, CLASS, -1, -1);
      if (id < 0) return -1;
      node_t *node = &p->grammar->nodes[id];
      bool negate = p->source[p->position] == '^', any = false;
      if (negate) p->position++;
      while (p->position < p->length && p->source[p->position] != ']')
        {
          int lo = byte (p), hi = lo;
          if (lo < 0) return -1;
          if (p->source[p->position] == '-' && p->position + 1 < p->length && p->source[p->position + 1] != ']')
            { p->position++; hi = byte (p); if (hi < 0) return -1; }
          if (hi < lo) return error_at (p, TS_ERR_PARSE, "descending PEG character range");
          for (int ch = lo; ch <= hi; ch++) node->bits[ch / 8] |= (unsigned char)(1u << (ch % 8));
          any = true;
        }
      if (!any || p->source[p->position] != ']') return error_at (p, TS_ERR_PARSE, "empty or unterminated PEG class");
      p->position++;
      if (negate) for (size_t i = 0; i < sizeof node->bits; i++) node->bits[i] ^= 255;
      return id;
    }
  if (c == '.') { p->position++; return new_node (p, ANY, -1, -1); }
  char name[65];
  if (!identifier (p, name)) return error_at (p, TS_ERR_PARSE, "expected a PEG expression");
  int id = new_node (p, REF, -1, -1);
  if (id < 0) return -1;
  p->grammar->nodes[id].offset = offset;
  p->grammar->nodes[id].text = strdup (name);
  if (!p->grammar->nodes[id].text) return error_at (p, TS_ERR_NOMEM, "out of memory");
  return id;
}
static int prefix (reader_t *p)
{
  skip (p);
  char c = p->source[p->position];
  if (c == '&' || c == '!')
    {
      p->position++;
      if (++p->depth > DEPTH_LIMIT) return error_at (p, TS_ERR_LIMIT, "PEG predicate nesting exceeds 128");
      int child = prefix (p); p->depth--;
      return child < 0 ? -1 : new_node (p, c == '&' ? AND : NOT, child, -1);
    }
  int result = primary (p);
  if (result < 0) return -1;
  skip (p); c = p->source[p->position];
  if (c == '*' || c == '+' || c == '?')
    { p->position++; result = new_node (p, c == '*' ? STAR : c == '+' ? PLUS : OPTIONAL, result, -1); }
  return result;
}
static bool rule_start (reader_t *p)
{
  reader_t copy = *p;
  char name[65];
  if (!identifier (&copy, name)) return false;
  skip (&copy);
  return copy.position + 1 < copy.length && copy.source[copy.position] == '<' && copy.source[copy.position + 1] == '-';
}
static int sequence (reader_t *p)
{
  int result = -1;
  for (;;)
    {
      skip (p);
      char c = p->source[p->position];
      if (!c || c == ')' || c == '/' || c == ';' || rule_start (p)) break;
      int child = prefix (p);
      if (child < 0) return -1;
      result = result < 0 ? child : new_node (p, SEQUENCE, result, child);
      if (result < 0) return -1;
    }
  return result < 0 ? new_node (p, EMPTY, -1, -1) : result;
}
static int expression (reader_t *p)
{
  int result = sequence (p);
  if (result < 0) return -1;
  skip (p);
  while (p->source[p->position] == '/')
    {
      p->position++;
      int child = sequence (p);
      if (child < 0) return -1;
      result = new_node (p, CHOICE, result, child);
      if (result < 0) return -1;
      skip (p);
    }
  return result;
}
static int find_rule (peg_t *g, const char *name)
{
  for (size_t i = 0; i < g->rule_count; i++) if (!strcmp (g->rules[i].name, name)) return (int)i;
  return -1;
}
/* Check all references reachable before consumption, including predicates and
 * nullable prefixes. Reject indirect/hidden left recursion before execution. */
static bool left_node (peg_t *, int, unsigned);
static bool left_rule (peg_t *g, int rule, unsigned depth)
{
  if (depth > DEPTH_LIMIT || g->rules[rule].visited == 1) return false;
  if (g->rules[rule].visited == 2) return true;
  g->rules[rule].visited = 1;
  if (!left_node (g, g->rules[rule].body, depth + 1)) return false;
  g->rules[rule].visited = 2;
  return true;
}
static bool left_node (peg_t *g, int id, unsigned depth)
{
  if (depth > DEPTH_LIMIT * 2) return false;
  node_t *n = &g->nodes[id];
  switch (n->kind)
    {
    case REF: return left_rule (g, n->rule, depth + 1);
    case SEQUENCE: return left_node (g, n->left, depth + 1)
                         && (!g->nodes[n->left].nullable || left_node (g, n->right, depth + 1));
    case CHOICE: return left_node (g, n->left, depth + 1) && left_node (g, n->right, depth + 1);
    case OPTIONAL: case STAR: case PLUS: case AND: case NOT: return left_node (g, n->left, depth + 1);
    default: return true;
    }
}
static int validate (reader_t *p)
{
  peg_t *g = p->grammar;
  for (size_t i = 0; i < g->node_count; i++)
    if (g->nodes[i].kind == REF)
      {
        g->nodes[i].rule = find_rule (g, g->nodes[i].text);
        if (g->nodes[i].rule < 0)
          {
            p->status = TS_ERR_PARSE;
            ts_error_set (p->error, p->status, 0, g->nodes[i].offset,
                          "undefined PEG rule '%s'", g->nodes[i].text);
            return -1;
          }
      }
  bool changed;
  do
    {
      changed = false;
      for (size_t i = 0; i < g->node_count; i++)
        {
          node_t *n = &g->nodes[i];
          if (n->nullable) continue;
          bool nullable = n->kind == EMPTY || (n->kind == LITERAL && !*n->text)
                          || n->kind == OPTIONAL || n->kind == STAR || n->kind == AND || n->kind == NOT
                          || (n->kind == REF && g->rules[n->rule].nullable)
                          || (n->kind == PLUS && g->nodes[n->left].nullable)
                          || (n->kind == SEQUENCE && g->nodes[n->left].nullable && g->nodes[n->right].nullable)
                          || (n->kind == CHOICE && (g->nodes[n->left].nullable || g->nodes[n->right].nullable));
          if (nullable) { n->nullable = true; changed = true; }
        }
      for (size_t i = 0; i < g->rule_count; i++)
        if (!g->rules[i].nullable && g->nodes[g->rules[i].body].nullable)
          { g->rules[i].nullable = true; changed = true; }
    }
  while (changed);
  for (size_t i = 0; i < g->node_count; i++)
    if ((g->nodes[i].kind == STAR || g->nodes[i].kind == PLUS) && g->nodes[g->nodes[i].left].nullable)
      return error_at (p, TS_ERR_PARSE, "PEG repetition body may match empty input");
  for (size_t i = 0; i < g->rule_count; i++)
    if (!left_rule (g, (int)i, 0))
      {
        p->status = TS_ERR_PARSE;
        ts_error_set (p->error, p->status, 0, g->rules[i].offset,
                      "PEG left recursion or excessive reference depth in '%s'", g->rules[i].name);
        return -1;
      }
  return 0;
}
static TS_Status
compile (TS_VM *vm, void *u, const TS_Value *a, size_t count, TS_Value *r, TS_Error *e)
{
  (void)u;
  const char *source;
  if (ts_std_argc (vm, count, 1, 1, e, "peg.compile") || ts_std_str (a, &source, e, "PEG source")) return TS_ERR_INVAL;
  size_t size = strlen (source);
  if (size > GRAMMAR_LIMIT) return failure (e, TS_ERR_LIMIT, 0, "PEG source exceeds 64 KiB");
  peg_t *g = calloc (1, sizeof *g);
  if (!g) return ts_std_oom (e);
  reader_t p = { .grammar = g, .source = source, .length = size, .error = e };
  skip (&p);
  while (p.position < p.length && !p.status)
    {
      if (g->rule_count == RULE_LIMIT) { error_at (&p, TS_ERR_LIMIT, "PEG exceeds 128 rules"); break; }
      size_t offset = p.position;
      char name[65];
      if (!identifier (&p, name)) { error_at (&p, TS_ERR_PARSE, "expected PEG rule name"); break; }
      if (find_rule (g, name) >= 0) { error_at (&p, TS_ERR_PARSE, "duplicate PEG rule"); break; }
      skip (&p);
      if (p.position + 1 >= p.length || source[p.position] != '<' || source[p.position + 1] != '-')
        { error_at (&p, TS_ERR_PARSE, "expected '<-' after PEG rule name"); break; }
      p.position += 2;
      rule_t *rule = &g->rules[g->rule_count++];
      rule->offset = offset;
      memcpy (rule->name, name, strlen (name) + 1);
      rule->body = expression (&p);
      if (rule->body < 0) break;
      skip (&p);
      if (source[p.position] == ';') { p.position++; skip (&p); }
    }
  if (!p.status && !g->rule_count) error_at (&p, TS_ERR_PARSE, "PEG contains no rules");
  if (!p.status) validate (&p);
  if (p.status) { destroy (g); return p.status; }
  TS_Status status = ts_value_make_handle (r, g, destroy, "PEG grammar", &tag);
  if (status) { destroy (g); return ts_std_oom (e); }
  return TS_OK;
}

typedef struct { int rule; size_t start, end, after; } capture_t;
typedef struct
{
  peg_t *grammar;
  const char *input;
  size_t length, steps, furthest;
  unsigned depth, calls;
  struct { int rule; size_t position; } active[DEPTH_LIMIT];
  capture_t *captures;
  size_t count, capacity;
  bool capture;
  TS_Status status;
  TS_Error *error;
} execution_t;
static int eval (execution_t *, int, size_t *);
static int fatal (execution_t *p, TS_Status status, size_t position, const char *message)
{
  if (!p->status) p->status = failure (p->error, status, position, message);
  return -1;
}
static int invoke (execution_t *p, int rule, size_t *position)
{
  if (p->calls == DEPTH_LIMIT) return fatal (p, TS_ERR_LIMIT, *position, "PEG rule recursion exceeds 128");
  for (unsigned i = 0; i < p->calls; i++)
    if (p->active[i].rule == rule && p->active[i].position == *position)
      return fatal (p, TS_ERR_PARSE, *position, "PEG left recursion during parse");
  size_t saved = p->count, at = *position;
  if (p->capture)
    {
      if (p->count == CAPTURE_LIMIT) return fatal (p, TS_ERR_LIMIT, at, "PEG capture count exceeds 65536");
      if (p->count == p->capacity)
        {
          size_t capacity = p->capacity ? p->capacity * 2 : 64;
          capture_t *next = realloc (p->captures, capacity * sizeof *next);
          if (!next) return fatal (p, TS_ERR_NOMEM, at, "out of memory");
          p->captures = next; p->capacity = capacity;
        }
      p->captures[p->count++] = (capture_t){ .rule = rule, .start = at };
    }
  p->active[p->calls].rule = rule; p->active[p->calls++].position = at;
  int result = eval (p, p->grammar->rules[rule].body, position);
  p->calls--;
  if (result == 1 && p->capture)
    { p->captures[saved].end = *position; p->captures[saved].after = p->count; }
  else if (result != 1) { p->count = saved; *position = at; }
  return result;
}
static int eval (execution_t *p, int id, size_t *position)
{
  size_t start = *position, saved = p->count;
  if (!p->steps--) return fatal (p, TS_ERR_LIMIT, start, "PEG execution step limit exceeded");
  if (p->depth == DEPTH_LIMIT * 2) return fatal (p, TS_ERR_LIMIT, start, "PEG execution nesting exceeds 256");
  p->depth++;
  node_t *n = &p->grammar->nodes[id];
  int result = 0;
  switch (n->kind)
    {
    case EMPTY: result = 1; break;
    case LITERAL:
      {
        size_t len = strlen (n->text);
        if (len <= p->length - start && !memcmp (p->input + start, n->text, len))
          { *position += len; result = 1; }
        else
          {
            size_t at = 0;
            while (at < len && start + at < p->length && p->input[start + at] == n->text[at]) at++;
            if (start + at > p->furthest) p->furthest = start + at;
          }
        break;
      }
    case CLASS: case ANY:
      if (start < p->length)
        {
          unsigned ch = (unsigned char)p->input[start];
          if (n->kind == ANY || (n->bits[ch / 8] & (1u << (ch % 8))))
            { (*position)++; result = 1; }
        }
      break;
    case REF: result = invoke (p, n->rule, position); break;
    case SEQUENCE:
      result = eval (p, n->left, position);
      if (result == 1) result = eval (p, n->right, position);
      break;
    case CHOICE:
      result = eval (p, n->left, position);
      if (!result) result = eval (p, n->right, position);
      break;
    case OPTIONAL:
      result = eval (p, n->left, position);
      if (!result) result = 1;
      break;
    case AND: case NOT:
      result = eval (p, n->left, position);
      *position = start; p->count = saved;
      if (result >= 0) result = n->kind == NOT ? !result : result;
      break;
    case STAR: case PLUS:
      {
        size_t iterations = 0;
        for (;;)
          {
            size_t before = *position;
            result = eval (p, n->left, position);
            if (result != 1)
              { if (!result) result = n->kind == STAR || iterations > 0; break; }
            if (*position == before)
              { result = fatal (p, TS_ERR_PARSE, before, "PEG repetition made no progress"); break; }
            iterations++;
          }
        break;
      }
    }
  p->depth--;
  if (*position > p->furthest) p->furthest = *position;
  if (result != 1) { *position = start; p->count = saved; }
  return result;
}
static ts_dom_t *tree (execution_t *p, size_t index, unsigned depth, size_t *bytes)
{
  capture_t *c = &p->captures[index];
  const char *name = p->grammar->rules[c->rule].name;
  size_t size = c->end - c->start;
  if (depth == DEPTH_LIMIT || size + strlen (name) > OUTPUT_LIMIT - *bytes)
    { fatal (p, TS_ERR_LIMIT, c->start, "PEG result tree exceeds depth/text limit"); return NULL; }
  *bytes += size + strlen (name);
  ts_dom_t *d = ts_dom_new_obj (), *children = ts_dom_new_arr ();
  char *text = malloc (size + 1);
  if (!d || !children || !text) goto oom;
  memcpy (text, p->input + c->start, size); text[size] = 0;
  for (size_t i = index + 1; i < c->after; i = p->captures[i].after)
    {
      ts_dom_t *child = tree (p, i, depth + 1, bytes);
      if (!child) goto fail;
      if (ts_dom_arr_push (children, child)) goto oom;
    }
  if (ts_dom_obj_set (d, "children", children)) { children = NULL; goto oom; }
  children = NULL;
  if (ts_dom_obj_set (d, "name", ts_dom_new_str (name))
      || ts_dom_obj_set (d, "text", ts_dom_new_str (text))
      || ts_dom_obj_set (d, "start", ts_dom_new_int ((int64_t)c->start))
      || ts_dom_obj_set (d, "end", ts_dom_new_int ((int64_t)c->end))) goto oom;
  free (text); return d;
oom:
  fatal (p, TS_ERR_NOMEM, c->start, "out of memory");
fail:
  free (text); ts_dom_free (children); ts_dom_free (d); return NULL;
}
static TS_Status
parse (TS_VM *vm, void *u, const TS_Value *a, size_t count, TS_Value *r, TS_Error *e)
{
  bool test = u != NULL;
  const char *input;
  int64_t steps = DEFAULT_STEPS;
  if (ts_std_argc (vm, count, 2, 3, e, "peg.parse")
      || ts_std_str (a + 1, &input, e, "PEG input")
      || (count == 3 && ts_std_int (a + 2, &steps, e, "PEG steps"))) return TS_ERR_INVAL;
  peg_t *g = ts_std_handle (a, &tag, e, "PEG grammar");
  if (!g) return TS_ERR_INVAL;
  if (g->closed) return ts_std_inval (e, "PEG grammar is closed");
  size_t size = strlen (input);
  if (size > OUTPUT_LIMIT || steps < 1 || steps > MAX_STEPS)
    return failure (e, TS_ERR_LIMIT, 0, "PEG input exceeds 1 MiB or steps outside 1..10000000");
  execution_t p = { .grammar = g, .input = input, .length = size, .steps = (size_t)steps,
                    .capture = !test, .error = e };
  size_t position = 0;
  int result = invoke (&p, 0, &position);
  if (result == 1 && position != size) { result = 0; p.furthest = position; }
  TS_Status status = p.status;
  if (!status && test) ts_std_ret_bool (r, result == 1);
  else if (!status && result != 1) status = failure (e, TS_ERR_PARSE, p.furthest, "PEG input did not match completely");
  else if (!status)
    {
      size_t bytes = 0;
      ts_dom_t *d = tree (&p, 0, 0, &bytes);
      status = d ? ts_dom_wrap (r, d, e) : p.status;
    }
  free (p.captures);
  return status;
}
static TS_Status
close_grammar (TS_VM *vm, void *u, const TS_Value *a, size_t count, TS_Value *r, TS_Error *e)
{
  (void)u; (void)r;
  if (ts_std_argc (vm, count, 1, 1, e, "peg.close")) return TS_ERR_INVAL;
  peg_t *g = ts_std_handle (a, &tag, e, "PEG grammar");
  if (!g) return TS_ERR_INVAL;
  release (g); return TS_OK;
}
static const TS_FuncDef functions[] = {
  { "compile", compile, NULL }, { "parse", parse, NULL },
  { "test", parse, (void *)1 }, { "close", close_grammar, NULL }
};
const TS_Module ts_std_peg_module = TS_MODULE_INIT ("std.peg", functions);
