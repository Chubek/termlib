/* Shared managed-binding and error-envelope semantics for every backend. */
typedef struct ts_envelope
{
  TS_Value value;
  TS_Error original;
  TS_Status status;
  char libname[129];
  struct ts_envelope *prev;
} ts_envelope;

TS_Status
ts_rt_lifetime_set (TS_VM *vm, const char *name, TS_Value *v,
                    unsigned line, TS_Error *error)
{
  ts_var_t *existing = vm && name ? vm_lookup (vm, name) : NULL;
  if (existing && existing->injected && v && v->type == TS_HANDLE &&
      existing->value.type == TS_HANDLE && existing->value.as.handle == v->as.handle)
    {
      existing->injected = false;
      ts_value_free (v); return TS_OK;
    }
  if (!vm || !name || !v || !strcmp (name, "$E") || vm_lookup (vm, name))
    { ts_error_set (error, TS_ERR_INVAL, 0, line, "lifetime binding '%s' already exists or is reserved", name ? name : "?"); return TS_ERR_INVAL; }
  TS_Status st = vm_define (vm, name, v, true, line, error);
  if (st == TS_OK)
    { ts_var_t *var = vm_lookup (vm, name); var->managed_prev = vm->managed; vm->managed = var; }
  return st;
}

TS_Status
ts_rt_flow_enter (TS_VM *vm, const TS_Value *value, TS_Status status, TS_Error *error)
{
  if (!vm || !value) { ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad flow frame"); return TS_ERR_INVAL; }
  if (status == TS_ERR_CANCELLED && vm->exit_requested) return status;
  ts_envelope *frame = calloc (1, sizeof *frame);
  if (!frame)
    {
      if (status != TS_OK) return status;
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory"); return TS_ERR_NOMEM;
    }
  frame->status = status;
  memcpy (frame->libname, vm->last_libname, sizeof frame->libname);
  if (error && status != TS_OK) frame->original = *error;
  if (status != TS_OK && frame->original.code == TS_OK)
    ts_error_set (&frame->original, status, 0, 0, "%s", ts_status_string (status));
  if (ts_value_copy (&frame->value, value) != TS_OK)
    { free (frame); ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory"); return TS_ERR_NOMEM; }
  frame->prev = vm->envelope; vm->envelope = frame;
  ts_error_clear (error);
  return TS_OK;
}

TS_Status
ts_rt_flow_leave (TS_VM *vm, TS_Status status, TS_Error *error)
{
  if (!vm || !vm->envelope) { ts_error_set (error, TS_ERR_INVAL, 0, 0, "unbalanced flow frame"); return TS_ERR_INVAL; }
  ts_envelope *frame = vm->envelope;
  if (frame->status != TS_OK)
    {
      status = frame->status;
      if (error) *error = frame->original;
      /* A successful exit requested by an error observer cannot recover an error. */
      vm->exit_requested = false;
    }
  vm->envelope = frame->prev;
  ts_value_free (&frame->value); free (frame);
  return status;
}

static TS_Status
ts_envelope_value (TS_VM *vm, TS_Value *out, unsigned line, TS_Error *error)
{
  if (!vm->envelope)
    { ts_error_set (error, TS_ERR_NOTFOUND, 0, line, "$E is only available inside a flow callback"); return TS_ERR_NOTFOUND; }
  /* This borrowed frame must never escape: expose its result through Value or
   * Library, and diagnostics through field lookup, rather than a raw handle. */
  return ts_value_make_string (out, vm->envelope->original.message);
}

TS_Status
ts_rt_field (TS_VM *vm, const char *name, const char *field, TS_Value *out,
             unsigned line, TS_Error *error)
{
  if (!vm || !name || !field || !out)
    { ts_error_set (error, TS_ERR_INVAL, 0, line, "bad field lookup"); return TS_ERR_INVAL; }
  ts_envelope *frame = vm->envelope;
  if (strcmp (name, "$E"))
    {
      TS_Value args[2] = {{0}, {0}};
      TS_Status st = ts_rt_get (vm, name, &args[0], line, error);
      if (st == TS_OK) st = ts_value_make_string (&args[1], field);
      if (st == TS_OK) st = ts_collection_call (vm, ts_vm_lookup (vm, "std.map"), "get", args, 2, out, error);
      ts_value_free (&args[0]); ts_value_free (&args[1]); return st;
    }
  if (!frame)
    { ts_error_set (error, TS_ERR_NOTFOUND, 0, line, "no field '%s->%s'", name, field); return TS_ERR_NOTFOUND; }
  if (!strcmp (field, "Msg")) return ts_value_make_string (out, frame->status == TS_OK ? "" : frame->original.message);
  if (!strcmp (field, "Libname")) return ts_value_make_string (out, frame->libname);
  if (!strcmp (field, "Code")) { ts_value_make_int (out, frame->status); return TS_OK; }
  if (!strcmp (field, "Errno")) { ts_value_make_int (out, frame->original.system_errno); return TS_OK; }
  if (!strcmp (field, "Offset")) { ts_value_make_int (out, (int64_t) frame->original.offset); return TS_OK; }
  if (!strcmp (field, "Value") || !strcmp (field, "Library")) return ts_value_copy (out, &frame->value);
  ts_error_set (error, TS_ERR_NOTFOUND, 0, line, "unknown envelope field '%s'", field);
  return TS_ERR_NOTFOUND;
}

TS_Status
ts_rt_finish (TS_VM *vm, TS_Status status, TS_Error *error)
{
  if (!vm) return TS_ERR_INVAL;
  while (vm->envelope) status = ts_rt_flow_leave (vm, status, error);
  while (vm->managed)
    {
      ts_var_t *var = vm->managed;
      vm->managed = var->managed_prev;
      ts_var_t **slot = &vm->vars;
      while (*slot && *slot != var) slot = &(*slot)->next;
      if (*slot) *slot = var->next;
      ts_value_free (&var->value); free (var->name); free (var);
    }
  if (status == TS_ERR_CANCELLED && vm->exit_requested && vm->exit_status == 0)
    { status = TS_OK; ts_error_clear (error); }
  vm->exit_requested = false;
  return status;
}

static TS_Status
eval_flow (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  TS_Error local;
  if (!error) { ts_error_clear (&local); error = &local; }
  TS_Value value = {0}, ignored = {0};
  TS_Status original = eval_node (vm, n->lhs, &value, error);
  TS_Error saved = *error;
  /* Successful exits bypass observers; ordinary errors enter the error path. */
  if (vm->exit_requested && original == TS_ERR_CANCELLED)
    { ts_value_free (&value); return original; }
  TS_Status st = ts_rt_flow_enter (vm, &value, original, error);
  if (st != TS_OK)
    { ts_value_free (&value); if (original != TS_OK) { *error = saved; return original; } return st; }
  if (original == TS_OK)
    st = eval_node (vm, n->rhs, &ignored, error);
  else
    {
      st = eval_node (vm, n->first, &ignored, error);
      ts_value_free (&ignored);
      if (st == TS_OK) st = eval_node (vm, n->otherwise, &ignored, error);
    }
  ts_value_free (&ignored);
  st = ts_rt_flow_leave (vm, st, error);
  if (st == TS_OK) { *out = value; value.type = TS_NIL; }
  ts_value_free (&value);
  return st;
}
