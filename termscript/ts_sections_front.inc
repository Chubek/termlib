/* Section syntax constructors and backend-independent semantic lowering.
 * Section calls are statically scheduled; no backend invents section semantics.
 */
void *
ts_mk_argexpr (void *expr)
{
  ts_node_t *n = expr;
  ts_arg_t *a;
  if (!n) return NULL;
  a = ts_arg_new (n->line);
  if (!a) { ts_node_free (n); return NULL; }
  switch (n->kind)
    {
    case N_VAR: a->name = n->name; n->name = NULL; break;
    case N_LIT_INT: a->tag = 1; a->number = n->number; break;
    case N_LIT_STR: a->tag = 2; a->string = n->string; n->string = NULL; break;
    case N_LIT_BOOL: a->tag = 3; a->boolean = n->boolean; break;
    case N_LIT_NIL: a->tag = 4; break;
    default: a->tag = 5; a->expr = n; return a;
    }
  ts_node_free (n);
  return a;
}

void *
ts_mk_design (int kind, char *name, char *other, void *body, unsigned line)
{
  const node_kind_t kinds[] = { N_SEQ, N_ALIAS, N_DEFINE, N_INTERFACE,
                               N_SECTION, N_CONST, N_MANAGED, N_LIT_STR };
  ts_node_t *n = ts_newnode (kinds[kind], line);
  if (!n) { free (name); free (other); ts_node_free (body); return NULL; }
  if (kind == 7) n->string = name;
  else n->name = name;
  n->module = other;
  if (kind == 3 || kind == 4)
    {
      ts_node_t *seq = body;
      if (!seq) { ts_node_free (n); return NULL; }
      n->first = seq->first; seq->first = seq->rhs = NULL;
      ts_node_free (seq);
    }
  else n->rhs = body;
  return n;
}

void *
ts_mk_flow (void *value, void *on_error, void *after_error,
            void *on_success, unsigned line)
{
  ts_node_t *n = ts_newnode (N_FLOW, line);
  if (!n) return NULL; /* Parse arena reclaims abandoned operands. */
  n->lhs = value; n->first = on_error;
  n->otherwise = after_error; n->rhs = on_success;
  return value ? n : NULL;
}

void *
ts_mk_field (char *name, char *field, unsigned line)
{
  ts_node_t *n = ts_newnode (N_FIELD, line);
  if (!n) { free (name); free (field); return NULL; }
  n->name = name; n->func = field;
  return n;
}

void *
ts_mk_not (void *expr, unsigned line)
{
  ts_node_t *n = ts_newnode (N_NOT, line);
  if (!n) return NULL;
  n->lhs = expr;
  return expr ? n : NULL;
}

static char *
ts_front_dup (const char *s)
{
  if (!s) return NULL;
  size_t len = strlen (s) + 1;
  char *p = malloc (len);
  if (p) memcpy (p, s, len);
  return p;
}

static ts_node_t *
ts_front_clone (const ts_node_t *s)
{
  if (!s) return NULL;
  ts_node_t *head = NULL, **tail = &head;
  for (; s; s = s->next)
    {
  ts_node_t *n = ts_node_new (s->kind, s->line);
  if (!n) { ts_node_free (head); return NULL; }
  n->number = s->number; n->boolean = s->boolean;
  n->name = ts_front_dup (s->name); n->module = ts_front_dup (s->module);
  n->func = ts_front_dup (s->func); n->string = ts_front_dup (s->string);
  n->first = ts_front_clone (s->first); n->cond = ts_front_clone (s->cond);
  n->otherwise = ts_front_clone (s->otherwise); n->lhs = ts_front_clone (s->lhs);
  n->rhs = s->kind == N_SEQ ? NULL : ts_front_clone (s->rhs);
  if ((s->name && !n->name) || (s->module && !n->module) ||
      (s->func && !n->func) || (s->string && !n->string) ||
      (s->first && !n->first) || (s->cond && !n->cond) ||
      (s->otherwise && !n->otherwise) || (s->lhs && !n->lhs) ||
      (s->kind != N_SEQ && s->rhs && !n->rhs))
    { ts_node_free (n); ts_node_free (head); return NULL; }
  n->nargs = s->nargs;
  if (s->nargs)
    {
      n->args = calloc (s->nargs, sizeof *n->args);
      if (!n->args) { n->nargs = 0; ts_node_free (n); ts_node_free (head); return NULL; }
      for (size_t i = 0; i < s->nargs; i++)
        {
          n->args[i] = s->args[i];
          n->args[i].name = ts_front_dup (s->args[i].name);
          n->args[i].string = ts_front_dup (s->args[i].string);
          n->args[i].expr = ts_front_clone (s->args[i].expr);
          if ((s->args[i].name && !n->args[i].name) ||
              (s->args[i].string && !n->args[i].string) ||
              (s->args[i].expr && !n->args[i].expr))
            { ts_node_free (n); ts_node_free (head); return NULL; }
        }
    }
  *tail = n; tail = &n->next;
    }
  return head;
}

enum { TS_MAX_SECTIONS = 128, TS_SECTION_DEPTH = 64, TS_LOWER_DEPTH = 256,
       TS_MAX_LOWER_NODES = 65536 };
typedef struct
{
  ts_node_t *sections[TS_MAX_SECTIONS], *directives[TS_MAX_SECTIONS];
  const char *declared[TS_MAX_SECTIONS];
  size_t nsections, ndirectives, ndeclared;
  size_t nodes, invocations;
  bool active[TS_MAX_SECTIONS];
  TS_Error *error;
} ts_section_context;

static TS_Status
ts_section_error (ts_section_context *c, ts_node_t *n, const char *msg,
                  const char *name)
{
  ts_error_set (c->error, TS_ERR_INVAL, 0, n->line, "%s '%s' (line %u)",
                msg, name ? name : "?", n->line);
  return TS_ERR_INVAL;
}

/* Resolve token substitutions on identifier slots only; string literals and
 * function names are never macro-expanded. Alias spelling is short -> long,
 * except builtin namespaces Global/Runtime whose executable names are G/R. */
static TS_Status
ts_section_name (ts_section_context *c, ts_node_t *n, char **slot)
{
  if (!*slot) return TS_OK;
  for (size_t depth = 0; depth <= c->ndirectives; depth++)
    {
      const char *replacement = NULL;
      for (size_t i = 0; i < c->ndirectives; i++)
        {
          ts_node_t *d = c->directives[i];
          const char *from = d->kind == N_ALIAS ? d->module : d->name;
          const char *to = d->kind == N_ALIAS ? d->name : d->module;
          if (d->kind == N_ALIAS && (!strcmp (to, "Global") || !strcmp (to, "Runtime")))
            { from = d->module; to = !strcmp (d->name, "Global") ? "G" : "R"; }
          if (!strcmp (*slot, from) && strcmp (from, to))
            { replacement = to; break; }
        }
      if (!replacement) return TS_OK;
      if (depth == c->ndirectives)
        return ts_section_error (c, n, "cyclic identifier substitution", *slot);
      char *copy = ts_front_dup (replacement);
      if (!copy) return TS_ERR_NOMEM;
      free (*slot); *slot = copy;
    }
  return TS_OK;
}

static const char *
ts_section_arg (ts_node_t *n)
{
  return n->nargs == 1 && n->args[0].tag == 2 ? n->args[0].string : NULL;
}

static TS_Status ts_section_walk (ts_section_context *, ts_node_t *, unsigned);

static TS_Status
ts_section_walk (ts_section_context *c, ts_node_t *n, unsigned depth)
{
  if (depth > TS_LOWER_DEPTH || c->nodes > TS_MAX_LOWER_NODES)
    { ts_error_set (c->error, TS_ERR_LIMIT, 0, 0, "section expansion depth exceeded"); return TS_ERR_LIMIT; }
  for (; n; n = n->next)
    {
      if (++c->nodes > TS_MAX_LOWER_NODES)
        { ts_error_set (c->error, TS_ERR_LIMIT, 0, n->line, "lowered program node limit exceeded"); return TS_ERR_LIMIT; }
      TS_Status st;
      if (n->kind == N_SECTION || n->kind == N_INTERFACE ||
          n->kind == N_ALIAS || n->kind == N_DEFINE)
        return ts_section_error (c, n, "directive must be at top level", n->name);
      if ((st = ts_section_name (c, n, &n->name)) != TS_OK ||
          (st = ts_section_name (c, n, &n->module)) != TS_OK) return st;
      for (size_t i = 0; i < n->nargs; i++)
        {
          if ((st = ts_section_name (c, n, &n->args[i].name)) != TS_OK) return st;
          if ((st = ts_section_walk (c, n->args[i].expr, depth + 1)) != TS_OK) return st;
        }
      if (n->kind == N_CALL && n->module && !strcmp (n->module, "R") &&
          !strcmp (n->func, "Inject") && n->nargs == 2)
        {
          ts_arg_t *target = &n->args[1];
          ts_node_t *qualified = target->tag == 5 ? target->expr : NULL;
          if (qualified && qualified->kind == N_CALL && !strcmp (qualified->module, "G") && !qualified->nargs)
            {
              target->string = ts_front_dup (qualified->func); target->tag = 2;
              ts_node_free (qualified); target->expr = NULL;
              if (!target->string) return TS_ERR_NOMEM;
            }
        }
      if (n->kind == N_CALL && n->module &&
          (!strcmp (n->module, "G") || !strcmp (n->module, "R")))
        {
          if (!strcmp (n->func, "Declare:Section") ||
              !strcmp (n->func, "Declare:Namespace") || !strcmp (n->func, "Declare:Symbol"))
            { n->kind = N_LIT_NIL; }
          else if (!strcmp (n->func, "OnStartup"))
            {
              const char *name = ts_section_arg (n);
              size_t i;
              if (!name) return ts_section_error (c, n, "section reference must be a constant symbol/string", n->func);
              for (i = 0; i < c->nsections; i++)
                if (!strcmp (name, c->sections[i]->name)) break;
              if (i == c->nsections) return ts_section_error (c, n, "undefined section", name);
              if (c->active[i]) return ts_section_error (c, n, "cyclic section invocation", name);
              if (c->invocations == TS_SECTION_DEPTH)
                { ts_error_set (c->error, TS_ERR_LIMIT, 0, n->line, "section invocation depth exceeded"); return TS_ERR_LIMIT; }
              c->invocations++;
              c->active[i] = true;
              n->kind = N_BLOCK; n->first = ts_front_clone (c->sections[i]->first);
              if (c->sections[i]->first && !n->first) return TS_ERR_NOMEM;
              st = ts_section_walk (c, n->first, depth + 1);
              c->active[i] = false;
              c->invocations--;
              if (st != TS_OK) return st;
              continue;
            }
        }
      if ((st = ts_section_walk (c, n->first, depth + 1)) != TS_OK ||
          (st = ts_section_walk (c, n->cond, depth + 1)) != TS_OK ||
          (st = ts_section_walk (c, n->otherwise, depth + 1)) != TS_OK ||
          (st = ts_section_walk (c, n->lhs, depth + 1)) != TS_OK ||
          (st = ts_section_walk (c, n->rhs, depth + 1)) != TS_OK) return st;
    }
  return TS_OK;
}

static TS_Status
ts_lower_sections (ts_node_t *prog, TS_Error *error)
{
  ts_section_context c = {0};
  ts_node_t *meta = NULL, *interface = NULL, *program = NULL;
  c.error = error;
  for (ts_node_t *n = prog->first; n; n = n->next)
    {
      if (n->kind == N_ALIAS || n->kind == N_DEFINE)
        {
          if (c.ndirectives == TS_MAX_SECTIONS) goto limit;
          for (size_t i = 0; i < c.ndirectives; i++)
            if (!strcmp (n->kind == N_ALIAS ? n->module : n->name,
                         c.directives[i]->kind == N_ALIAS ? c.directives[i]->module : c.directives[i]->name))
              return ts_section_error (&c, n, "duplicate alias/definition", n->name);
          c.directives[c.ndirectives++] = n;
        }
      else if (n->kind == N_INTERFACE)
        { if (meta) return ts_section_error (&c, n, "duplicate interface", ".interface"); meta = n; }
      else if (n->kind == N_SECTION)
        {
          if (c.nsections == TS_MAX_SECTIONS) goto limit;
          for (size_t i = 0; i < c.nsections; i++)
            if (!strcmp (n->name, c.sections[i]->name))
              return ts_section_error (&c, n, "duplicate section", n->name);
          c.sections[c.nsections++] = n;
          if (!strcmp (n->name, "Interface")) interface = n;
          if (!strcmp (n->name, "Program")) program = n;
        }
    }
  if (meta)
    for (ts_node_t *s = meta->first; s; s = s->next)
      {
        ts_node_t *n = s->kind == N_EXPR_STMT ? s->rhs : NULL;
        if (!n || n->kind != N_CALL || !ts_section_arg (n) ||
            strcmp (n->func, "Declare:Section"))
          return ts_section_error (&c, s, "expected section declaration", ".interface");
        if (c.ndeclared == TS_MAX_SECTIONS) goto limit;
        const char *name = ts_section_arg (n);
        for (size_t i = 0; i < c.ndeclared; i++)
          if (!strcmp (name, c.declared[i])) return ts_section_error (&c, n, "duplicate section declaration", name);
        c.declared[c.ndeclared++] = name;
      }
  for (size_t i = 0; i < c.ndeclared; i++)
    {
      size_t j;
      for (j = 0; j < c.nsections; j++) if (!strcmp (c.declared[i], c.sections[j]->name)) break;
      if (j == c.nsections) return ts_section_error (&c, meta, "declared section has no body", c.declared[i]);
    }
  if (!c.nsections && !c.ndirectives && !meta)
    return ts_section_walk (&c, prog->first, 0);
  if (c.nsections)
    {
      if (!program) return ts_section_error (&c, prog, "missing entry section", "Program");
      for (ts_node_t *n = prog->first; n; n = n->next)
        if (n->kind != N_ALIAS && n->kind != N_DEFINE && n->kind != N_INTERFACE && n->kind != N_SECTION)
          return ts_section_error (&c, n, "statement outside section", "Program");
      if (meta)
        for (size_t i = 0; i < c.nsections; i++)
          {
            size_t j;
            for (j = 0; j < c.ndeclared; j++) if (!strcmp (c.sections[i]->name, c.declared[j])) break;
            if (j == c.ndeclared) return ts_section_error (&c, c.sections[i], "undeclared section", c.sections[i]->name);
          }
      /* Validate every body, including unreachable sections, using clones. */
      for (size_t i = 0; i < c.nsections; i++)
        {
          ts_node_t *copy = ts_front_clone (c.sections[i]->first);
          if (c.sections[i]->first && !copy) return TS_ERR_NOMEM;
          c.active[i] = true;
          TS_Status st = ts_section_walk (&c, copy, 0);
          c.active[i] = false;
          ts_node_free (copy);
          if (st != TS_OK) return st;
        }
    }
  ts_node_t *head = NULL, **tail = &head;
  if (c.nsections)
    {
      if (interface) { *tail = ts_front_clone (interface->first); if (interface->first && !*tail) return TS_ERR_NOMEM; while (*tail) tail = &(*tail)->next; }
      *tail = ts_front_clone (program->first);
      if (program->first && !*tail) { ts_node_free (head); return TS_ERR_NOMEM; }
    }
  else
    {
      for (ts_node_t *n = prog->first; n; n = n->next)
        if (n->kind != N_ALIAS && n->kind != N_DEFINE && n->kind != N_INTERFACE)
          {
            ts_node_t *next = n->next;
            n->next = NULL;
            ts_node_t *copy = ts_front_clone (n);
            n->next = next;
            if (!copy) { ts_node_free (head); return TS_ERR_NOMEM; }
            ts_node_free (copy->next); copy->next = NULL;
            *tail = copy; tail = &copy->next;
          }
    }
  TS_Status st = ts_section_walk (&c, head, 0);
  if (st != TS_OK) { ts_node_free (head); return st; }
  ts_node_free (prog->first); prog->first = head; prog->rhs = NULL;
  return TS_OK;
limit:
  ts_error_set (error, TS_ERR_LIMIT, 0, 0, "section/directive table limit exceeded");
  return TS_ERR_LIMIT;
}
