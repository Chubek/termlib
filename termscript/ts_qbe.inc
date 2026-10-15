/* Direct AST lowering to QBE. Included after the shared AOT import helpers.
 * Value slots are allocated once in the entry block and released on every
 * exit, including runtime errors. No Termscript source is embedded. */
typedef struct
{
  ts_cg_t body, data, slots, cleanup;
  unsigned id;
  bool dt;
  unsigned failure;
} ts_qg;

static unsigned
ts_qid (ts_qg *g) { return ++g->id; }

static unsigned
ts_qstr (ts_qg *g, const char *s)
{
  unsigned id = ts_qid (g);
  const unsigned char *p = (const unsigned char *) s;
  ts_cg_fmt (&g->data, "data $s%u = { b ", id);
  for (; *p; p++) ts_cg_fmt (&g->data, "%u, b ", *p);
  ts_cg_str (&g->data, "0 }\n");
  return id;
}

static unsigned
ts_qslot (ts_qg *g)
{
  unsigned id = ts_qid (g);
  ts_cg_fmt (&g->slots, "%%v%u =l alloc8 %zu\nstorew 0, %%v%u\n",
             id, sizeof (TS_Value), id);
  ts_cg_fmt (&g->cleanup, "call $%s(l %%v%u)\n",
             g->dt ? "dt_termvalue_free" : "ts_value_free", id);
  return id;
}

static const char *
ts_qfn (ts_qg *g, const char *ts, const char *dt) { return g->dt ? dt : ts; }

static void
ts_qcheck (ts_qg *g, unsigned result)
{
  unsigned label = ts_qid (g);
  if (g->failure)
    ts_cg_fmt (&g->body, "jnz %%r%u, @b%u, @b%u\n@b%u\n", result, g->failure, label, label);
  else ts_cg_fmt (&g->body, "jnz %%r%u, @fail, @b%u\n@b%u\n", result, label, label);
}

static void
ts_qfail (ts_qg *g)
{
  if (g->failure) ts_cg_fmt (&g->body, "jmp @b%u\n", g->failure);
  else ts_cg_str (&g->body, "jmp @fail\n");
}

static void
ts_qstep (ts_qg *g)
{
  unsigned r = ts_qid (g);
  ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l %%err)\n", r,
             ts_qfn (g, "ts_rt_step", "dt_rt_step"));
  ts_qcheck (g, r);
}

static void
ts_qfree (ts_qg *g, unsigned v)
{
  ts_cg_fmt (&g->body, "call $%s(l %%v%u)\n",
             ts_qfn (g, "ts_value_free", "dt_termvalue_free"), v);
}

static void
ts_qget (ts_qg *g, const char *name, unsigned out, unsigned line)
{
  unsigned s = ts_qstr (g, name), r = ts_qid (g);
  ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l $s%u, l %%v%u, w %u, l %%err)\n",
             r, ts_qfn (g, "ts_rt_get", "dt_rt_get"), s, out, line);
  ts_qcheck (g, r);
}

static void
ts_qlit (ts_qg *g, unsigned out, int type, int64_t number, const char *str)
{
  if (type == TS_STRING)
    {
      unsigned s = ts_qstr (g, str), r = ts_qid (g);
      if (g->dt)
        ts_cg_fmt (&g->body, "%%r%u =w call $dt_rt_mkstring(l %%v%u, l $s%u, l %%err)\n", r, out, s);
      else
        {
          unsigned ok = ts_qid (g), bad = ts_qid (g);
          unsigned msg = ts_qstr (g, "out of memory");
          ts_cg_fmt (&g->body, "%%r%u =w call $ts_value_make_string(l %%v%u, l $s%u)\n", r, out, s);
          ts_cg_fmt (&g->body, "jnz %%r%u, @b%u, @b%u\n@b%u\n", r, bad, ok, bad);
          ts_cg_fmt (&g->body, "call $ts_error_set(l %%err, w %d, w %d, l 0, l $s%u, ...)\n",
                     TS_ERR_NOMEM, ENOMEM, msg);
          ts_qfail (g); ts_cg_fmt (&g->body, "@b%u\n", ok);
          return;
        }
      ts_qcheck (g, r);
    }
  else
    {
      unsigned p = ts_qid (g);
      ts_cg_fmt (&g->body, "storew %d, %%v%u\n%%p%u =l add %%v%u, %zu\nstorel %lld, %%p%u\n",
                 type, out, p, out, offsetof (TS_Value, as), (long long) number, p);
    }
}

static void ts_qexpr (ts_qg *, ts_node_t *, unsigned);
static void ts_qlist (ts_qg *, ts_node_t *);

static void
ts_qexpr (ts_qg *g, ts_node_t *n, unsigned out)
{
  if (!n) return;
  ts_qstep (g);
  switch (n->kind)
    {
    case N_VAR: ts_qget (g, n->name, out, n->line); break;
    case N_LIT_INT: ts_qlit (g, out, TS_INT, n->number, NULL); break;
    case N_LIT_STR: ts_qlit (g, out, TS_STRING, 0, n->string); break;
    case N_LIT_BOOL: ts_qlit (g, out, TS_BOOL, n->boolean, NULL); break;
    case N_LIT_NIL: ts_qlit (g, out, TS_NIL, 0, NULL); break;
    case N_BLOCK: ts_qlist (g, n->first); break;
    case N_FIELD:
      {
        unsigned name = ts_qstr (g, n->name), field = ts_qstr (g, n->func), r = ts_qid (g);
        ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l $s%u, l $s%u, l %%v%u, w %u, l %%err)\n",
                   r, ts_qfn (g, "ts_rt_field", "dt_rt_field"), name, field, out, n->line);
        ts_qcheck (g, r);
      }
      break;
    case N_NOT:
      {
        unsigned v = ts_qslot (g), r = ts_qid (g), p = ts_qid (g);
        ts_qexpr (g, n->lhs, v);
        ts_cg_fmt (&g->body, "%%truth%u =w call $%s(l %%v%u)\n%%bool%u =w and %%truth%u, 255\n%%not%u =w ceqw %%bool%u, 0\nstorew %d, %%v%u\n%%p%u =l add %%v%u, %zu\nstoreb %%not%u, %%p%u\n",
                   r, ts_qfn (g, "ts_value_truthy", "dt_rt_truthy"), v, r, r,
                   r, r, TS_BOOL, out, p, out, offsetof (TS_Value, as), r, p);
        ts_qfree (g, v);
      }
      break;
    case N_FLOW:
      {
        unsigned parent = g->failure, bad = ts_qid (g), join = ts_qid (g);
        unsigned yes = ts_qid (g), no = ts_qid (g), cb_bad = ts_qid (g), cb_join = ts_qid (g);
        unsigned original = ts_qid (g), status = ts_qid (g), ignored = ts_qslot (g), r;
        ts_cg_fmt (&g->slots, "%%original%u =l alloc4 4\n%%status%u =l alloc4 4\n", original, status);
        ts_cg_fmt (&g->body, "storew 0, %%original%u\nstorew 0, %%status%u\n", original, status);
        g->failure = bad; ts_qexpr (g, n->lhs, out); g->failure = parent;
        ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n%%error%u =w loadw %%err\nstorew %%error%u, %%original%u\njmp @b%u\n@b%u\n",
                   join, bad, bad, bad, original, join, join);
        r = ts_qid (g);
        ts_cg_fmt (&g->body, "%%orig%u =w loadw %%original%u\n%%r%u =w call $%s(l %%vm, l %%v%u, w %%orig%u, l %%err)\n",
                   original, original, r, ts_qfn (g, "ts_rt_flow_enter", "dt_rt_flow_enter"), out, original);
        ts_qcheck (g, r);
        ts_cg_fmt (&g->body, "jnz %%orig%u, @b%u, @b%u\n@b%u\n", original, no, yes, yes);
        g->failure = cb_bad;
        ts_qexpr (g, n->rhs, ignored);
        ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n", cb_join, no);
        ts_qexpr (g, n->first, ignored); ts_qfree (g, ignored);
        ts_qexpr (g, n->otherwise, ignored);
        g->failure = parent;
        ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n%%error%u =w loadw %%err\nstorew %%error%u, %%status%u\njmp @b%u\n@b%u\n",
                   cb_join, cb_bad, cb_bad, cb_bad, status, cb_join, cb_join);
        ts_qfree (g, ignored); r = ts_qid (g);
        ts_cg_fmt (&g->body, "%%st%u =w loadw %%status%u\n%%r%u =w call $%s(l %%vm, w %%st%u, l %%err)\n",
                   status, status, r, ts_qfn (g, "ts_rt_flow_leave", "dt_rt_flow_leave"), status);
        ts_qcheck (g, r);
      }
      break;
    case N_OR:
      {
        unsigned r = ts_qid (g), rhs = ts_qid (g), end = ts_qid (g);
        ts_qexpr (g, n->lhs, out);
        ts_cg_fmt (&g->body, "%%bool%u =w call $%s(l %%v%u)\n%%r%u =w and %%bool%u, 255\njnz %%r%u, @b%u, @b%u\n@b%u\n",
                   r, ts_qfn (g, "ts_value_truthy", "dt_rt_truthy"), out, r, r, r, end, rhs, rhs);
        ts_qfree (g, out);
        ts_qexpr (g, n->rhs, out);
        ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n", end, end);
      }
      break;
    case N_CALL:
      {
        unsigned mod = ts_qslot (g), args = ts_qid (g), r, mn, fn;
        size_t i;
        unsigned *av = calloc (n->nargs ? n->nargs : 1, sizeof *av);
        if (!av) { g->body.failed = true; break; }
        /* A contiguous argument array, with separately named pointers so
         * every owned value participates in the common cleanup path. */
        ts_cg_fmt (&g->slots, "%%a%u =l alloc8 %zu\n", args,
                   (n->nargs ? n->nargs : 1) * sizeof (TS_Value));
        for (i = 0; i < n->nargs; i++)
          {
            av[i] = ts_qid (g);
            ts_cg_fmt (&g->slots, "%%v%u =l add %%a%u, %zu\nstorew 0, %%v%u\n",
                       av[i], args, i * sizeof (TS_Value), av[i]);
            ts_cg_fmt (&g->cleanup, "call $%s(l %%v%u)\n",
                       ts_qfn (g, "ts_value_free", "dt_termvalue_free"), av[i]);
          }
        ts_qget (g, n->module, mod, n->line);
        for (i = 0; i < n->nargs; i++)
          {
            ts_arg_t *a = &n->args[i];
            if (a->tag == 5) ts_qexpr (g, a->expr, av[i]);
            else if (!a->tag) ts_qget (g, a->name, av[i], n->line);
            else ts_qlit (g, av[i], a->tag == 1 ? TS_INT : a->tag == 2 ? TS_STRING : a->tag == 3 ? TS_BOOL : TS_NIL,
                          a->tag == 1 ? a->number : a->boolean, a->string);
          }
        mn = ts_qstr (g, n->module); fn = ts_qstr (g, n->func); r = ts_qid (g);
        ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l %%v%u, l $s%u, l $s%u, l %%a%u, l %zu, l %%v%u, w %u, l %%err)\n",
                   r, ts_qfn (g, "ts_rt_call", "dt_rt_call"), mod, mn, fn, args, n->nargs, out, n->line);
        ts_qfree (g, mod);
        for (i = 0; i < n->nargs; i++) ts_qfree (g, av[i]);
        free (av);
        ts_qcheck (g, r);
      }
      break;
    default: g->body.failed = true; break;
    }
}

static void
ts_qlist (ts_qg *g, ts_node_t *s)
{
  for (; s && !g->body.failed; s = s->next)
    {
      unsigned v = ts_qslot (g), r;
      if (s->kind == N_WHILE || s->kind == N_IF)
        {
          unsigned top = ts_qid (g), yes = ts_qid (g), no = ts_qid (g), end = ts_qid (g);
          ts_qstep (g);
          ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n", top, top);
          ts_qexpr (g, s->cond, v); r = ts_qid (g);
          ts_cg_fmt (&g->body, "%%bool%u =w call $%s(l %%v%u)\n%%r%u =w and %%bool%u, 255\n", r,
                     ts_qfn (g, "ts_value_truthy", "dt_rt_truthy"), v, r, r);
          ts_qfree (g, v);
          ts_cg_fmt (&g->body, "jnz %%r%u, @b%u, @b%u\n@b%u\n", r, yes, no, yes);
          ts_qlist (g, s->first);
          ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n", s->kind == N_WHILE ? top : end, no);
          if (s->kind == N_IF) ts_qlist (g, s->otherwise);
          ts_cg_fmt (&g->body, "jmp @b%u\n@b%u\n", end, end);
        }
      else
        {
          ts_qstep (g); ts_qexpr (g, s->rhs, v);
          if (s->kind == N_CONST || s->kind == N_ASSIGN || s->kind == N_MANAGED)
            {
              unsigned name = ts_qstr (g, s->name);
              r = ts_qid (g);
              if (s->kind == N_MANAGED)
                ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l $s%u, l %%v%u, w %u, l %%err)\n",
                           r, ts_qfn (g, "ts_rt_lifetime_set", "dt_rt_lifetime_set"), name, v, s->line);
              else ts_cg_fmt (&g->body, "%%r%u =w call $%s(l %%vm, l $s%u, l %%v%u, w %u, w %u, l %%err)\n",
                             r, ts_qfn (g, "ts_rt_set", "dt_rt_set"), name, v, s->kind == N_CONST, s->line);
              ts_qcheck (g, r);
            }
          ts_qfree (g, v);
        }
    }
}

const char *
ts_vm_compile_to_qbe (TS_VM *vm, const char *source, bool dt_mode, TS_Error *error)
{
  ts_node_t *prog = NULL;
  ts_qg g = {0};
  ts_cg_t unit = {0};
  struct ts_cg_paths paths = {0};
  char *stack[64] = {0};
  size_t i;
  if (!vm || !source) { ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments"); return NULL; }
  if (ts_parse_program (source, &prog, error) != TS_OK) return NULL;
  for (i = 0; i < vm->nspaths; i++) ts_cg_paths_add (&paths, vm->spaths[i]);
  ts_cg_inline_chain (&prog->first, &paths, stack, 0, 0);
  ts_cg_paths_free (&paths);
  g.dt = dt_mode;
  ts_qlist (&g, prog->first);
  ts_node_free (prog);
  ts_cg_str (&unit, g.data.buf ? g.data.buf : "");
  ts_cg_str (&unit, "export function w $ts_qbe_run(l %vm, l %err) {\n@entry\n");
  ts_cg_str (&unit, g.slots.buf ? g.slots.buf : "");
  ts_cg_str (&unit, g.body.buf ? g.body.buf : "");
  ts_cg_str (&unit, "jmp @success\n@success\n%status0 =w copy 0\njmp @cleanup\n@fail\n%code =w loadw %err\n%bad =w ceqw %code, 0\n%status1 =w add %code, %bad\njmp @cleanup\n@cleanup\n%status =w phi @success %status0, @fail %status1\n");
  ts_cg_str (&unit, g.cleanup.buf ? g.cleanup.buf : "");
  ts_cg_fmt (&unit, "%%final =w call $%s(l %%vm, w %%status, l %%err)\nret %%final\n}\n",
             ts_qfn (&g, "ts_rt_finish", "dt_rt_finish"));
  free (g.body.buf); free (g.data.buf); free (g.slots.buf);
  free (g.cleanup.buf);
  if (unit.failed || g.body.failed || g.data.failed || g.slots.failed || g.cleanup.failed)
    { free (unit.buf); ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "QBE emission failed"); return NULL; }
  free (vm->compiled); vm->compiled = unit.buf;
  return vm->compiled;
}
