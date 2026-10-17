/* ts_runtime.c -- standalone Termscript runtime (no Termlib dependency).
 *
 * Implements termscript/termscript.h: the embedding API (TS_VM lifecycle,
 * run/compile) and the native extension ABI (TS_Module tables).  Only
 * standard C headers plus termscript.h are included; linking requires
 * libc, pthreads, and the vendored NovoParse runtime.
 *
 * Formal grammar: termscript/Termscript.ng, compiled by NovoParse into a
 * parser descriptor.  termscript/ts_parser_ng.c adapts the NovoParse syntax
 * tree into the ts_node_t AST below through the ts_* constructors; the
 * tree-walking evaluator and AOT backends then operate on that tree.
 *
 * Core language subset:
 *   program  := stmt*
 *   stmt     := "const" IDENT "=" expr ";"
 *             | IDENT "=" expr ";"                (assign/declare)
 *             | "while" expr "do" stmt* "end"
 *             | "if" expr "do" stmt* ["else" stmt*] "end"
 *             | expr ";"
 *   expr     := unary ("or" unary)*
 *   unary    := "&" IDENT | primary
 *   primary  := IDENT ":" IDENT args | IDENT | STRING | NUMBER
 *             | "true" | "false" | "nil"
 *   args     := arg*   (each STRING | NUMBER | IDENT | "&" IDENT)
 *
 * Values: nil, bool, int, string, handle (native module or extension
 * data).  Falsiness: nil, false, 0 and "" are false; all else true.
 * `or` short-circuits.  G:die halts with TS_ERR_CANCELLED.
 * Execution is bounded by TS_MAX_STEPS (infinite loops fail). */
#define _POSIX_C_SOURCE 200809L

#include "termscript.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_MAX_STEPS (1u << 20)
#define TS_MAX_FILE (1u << 20)

#if defined(TERMSCRIPT_HAVE_JITERATI)
int ts_jiterati_available (void);
int ts_jiterati_binary_i32 (int opcode, int32_t lhs, int32_t rhs, int32_t *out,
                            char *error, size_t error_size);
int ts_jiterati_add_i32 (int32_t lhs, int32_t rhs, int32_t *out, char *error,
                         size_t error_size);
#endif

/* ---------------- status and errors ---------------- */

const char *
ts_status_string (TS_Status status)
{
  switch (status)
    {
    case TS_OK:
      return "OK";
    case TS_ERR_INVAL:
      return "INVALID_ARGUMENT";
    case TS_ERR_NOMEM:
      return "NO_MEMORY";
    case TS_ERR_SYSTEM:
      return "SYSTEM";
    case TS_ERR_NOTFOUND:
      return "NOT_FOUND";
    case TS_ERR_PARSE:
      return "PARSE";
    case TS_ERR_UNSUPPORTED:
      return "UNSUPPORTED";
    case TS_ERR_EOF:
      return "EOF";
    case TS_ERR_TIMEOUT:
      return "TIMEOUT";
    case TS_ERR_WOULD_BLOCK:
      return "WOULD_BLOCK";
    case TS_ERR_CANCELLED:
      return "CANCELLED";
    case TS_ERR_LIMIT:
      return "LIMIT";
    default:
      return "UNKNOWN";
    }
}

void
ts_error_set (TS_Error *error, TS_Status code, int system_errno, size_t offset,
              const char *fmt, ...)
{
  va_list ap;
  if (!error)
    return;
  error->code = code;
  error->system_errno = system_errno;
  error->offset = offset;
  if (!fmt)
    {
      error->message[0] = '\0';
      return;
    }
  va_start (ap, fmt);
  vsnprintf (error->message, sizeof error->message, fmt, ap);
  va_end (ap);
}

void
ts_error_clear (TS_Error *error)
{
  if (!error)
    return;
  error->code = TS_OK;
  error->system_errno = 0;
  error->offset = 0;
  error->message[0] = '\0';
}

void
ts_compile_options_init (TS_CompileOptions *options)
{
  if (!options)
    return;
  memset (options, 0, sizeof *options);
  options->struct_size = sizeof *options;
  options->version = TS_COMPILE_OPTIONS_VERSION;
  options->backend = TS_BACKEND_C;
}

const char *
ts_backend_name (TS_Backend backend)
{
  switch (backend)
    {
    case TS_BACKEND_C:
      return "c";
    case TS_BACKEND_VM:
      return "vm";
    case TS_BACKEND_QBE:
      return "qbe";
    case TS_BACKEND_JITERATI:
      return "jiterati";
    default:
      return "unknown";
    }
}

bool
ts_backend_available (TS_Backend backend)
{
  switch (backend)
    {
    case TS_BACKEND_VM:
    case TS_BACKEND_C:
    case TS_BACKEND_QBE:
      return true;
    case TS_BACKEND_JITERATI:
#if defined(TERMSCRIPT_HAVE_JITERATI)
      return ts_jiterati_available () != 0;
#else
      return false;
#endif
    default:
      return false;
    }
}

/* ---------------- value handles ---------------- */

/* Identity token for module handles (address used for tag checks). */
static const char ts_module_tag_id = 0;

struct TS_Handle
{
  int refcount;
  void *ptr;
  void (*free_fn) (void *ptr);
  const void *tag;
  char desc[96];
};

static TS_Handle *
ts_handle_new (void *ptr, void (*free_fn) (void *), const void *tag,
               const char *desc)
{
  TS_Handle *h = calloc (1, sizeof *h);
  if (!h)
    return NULL;
  h->refcount = 1;
  h->ptr = ptr;
  h->free_fn = free_fn;
  h->tag = tag;
  snprintf (h->desc, sizeof h->desc, "%s", desc ? desc : "<handle>");
  return h;
}

static TS_Handle *
ts_handle_new_module (const TS_Module *mod)
{
  char desc[96];
  snprintf (desc, sizeof desc, "<module %s>",
            mod && mod->name ? mod->name : "?");
  return ts_handle_new ((void *)mod, NULL, &ts_module_tag_id, desc);
}

static const TS_Module *
ts_handle_module (const TS_Handle *h)
{
  if (!h || h->tag != &ts_module_tag_id)
    return NULL;
  return (const TS_Module *)h->ptr;
}

static void
ts_handle_retain (TS_Handle *h)
{
  if (h)
    h->refcount++;
}

static void
ts_handle_release (TS_Handle *h)
{
  if (!h)
    return;
  if (--h->refcount > 0)
    return;
  if (h->free_fn)
    h->free_fn (h->ptr);
  free (h);
}

/* ---------------- VM ---------------- */

typedef struct ts_var
{
  char *name;
  TS_Value value;
  bool is_const;
  bool injected;
  struct ts_var *managed_prev;
  struct ts_var *next;
} ts_var_t;

typedef struct ts_frame
{
  ts_var_t *base, *managed, *globals;
  TS_Value result;
  bool returning;
  struct ts_frame *previous;
} ts_frame;

struct TS_VM
{
  ts_var_t *vars;
  const TS_Module *modules[TS_MAX_MODULES];
  size_t nmodules;
  /* Guards modules/nmodules so registration may race running scripts
   * (mirrors the old global-registry lock); never held across callbacks. */
  pthread_mutex_t mod_mutex;
  void *userdata;
  char *output;
  size_t olen, ocap;
  unsigned long steps;
  char *compiled;
  /* Import machinery (see ts_vm_import): ordered search directories plus
   * a stack of in-progress import paths for cycle detection. */
  char **spaths;
  size_t nspaths, spaths_cap;
  char **istack;
  size_t nistack, istack_cap;
  ts_var_t *managed;
  struct ts_envelope *envelope;
  bool exit_requested;
  int64_t exit_status;
  bool jit;
  char last_libname[129];
  ts_frame *frame;
  unsigned call_depth;
  char **argv;
  size_t argc;
  int main_status;
};

static const TS_Module *
ts_vm_lookup (TS_VM *vm, const char *name)
{
  size_t i;
  const TS_Module *found = NULL;
  if (!vm || !name)
    return NULL;
  pthread_mutex_lock (&vm->mod_mutex);
  for (i = 0; i < vm->nmodules; i++)
    if (vm->modules[i]->name && strcmp (vm->modules[i]->name, name) == 0)
      {
        found = vm->modules[i];
        break;
      }
  pthread_mutex_unlock (&vm->mod_mutex);
  return found;
}

TS_Status
ts_module_validate (const TS_Module *module, TS_Error *error)
{
  size_t i, j;
  if (!module)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad module");
      return TS_ERR_INVAL;
    }
  if (module->struct_size != sizeof *module
      || module->abi_version != TS_NATIVE_ABI_VERSION)
    {
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "incompatible native module ABI (size %u, version %u)",
                    module->struct_size, module->abi_version);
      return TS_ERR_UNSUPPORTED;
    }
  for (i = 0; i < sizeof module->reserved / sizeof module->reserved[0]; i++)
    if (module->reserved[i])
      {
        ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "native module reserved fields must be zero");
        return TS_ERR_INVAL;
      }
  if (!module->name || !module->name[0]
      || (module->func_count && !module->funcs))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad module name or table");
      return TS_ERR_INVAL;
    }
  if (module->func_count > TS_MAX_MODULE_FUNCS)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                    "module '%s' has too many functions", module->name);
      return TS_ERR_LIMIT;
    }
  for (i = 0; i < module->func_count; i++)
    {
      const TS_FuncDef *f = &module->funcs[i];
      if (!f->name || !f->name[0] || !f->func)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0,
                        "module '%s' has invalid function at index %zu",
                        module->name, i);
          return TS_ERR_INVAL;
        }
      for (j = 0; j < i; j++)
        if (strcmp (module->funcs[j].name, f->name) == 0)
          {
            ts_error_set (error, TS_ERR_INVAL, 0, 0,
                          "module '%s' has duplicate function '%s'",
                          module->name, f->name);
            return TS_ERR_INVAL;
          }
    }
  return TS_OK;
}

void
ts_module_init (TS_Module *module, const char *name, const TS_FuncDef *funcs,
                size_t func_count)
{
  if (!module)
    return;
  memset (module, 0, sizeof *module);
  module->struct_size = sizeof *module;
  module->abi_version = TS_NATIVE_ABI_VERSION;
  module->name = name;
  module->funcs = funcs;
  module->func_count = func_count;
}

uint32_t
ts_native_abi_version (void)
{
  return TS_NATIVE_ABI_VERSION;
}

const TS_FuncDef *
ts_module_find_func (const TS_Module *module, const char *name)
{
  size_t i;
  if (!module || !name)
    return NULL;
  for (i = 0; i < module->func_count; i++)
    if (strcmp (module->funcs[i].name, name) == 0)
      return &module->funcs[i];
  return NULL;
}

TS_Status
ts_vm_register_module (TS_VM *vm, const TS_Module *module, TS_Error *error)
{
  size_t i;
  TS_Status st;
  if (!vm)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad vm");
      return TS_ERR_INVAL;
    }
  st = ts_module_validate (module, error);
  if (st != TS_OK)
    return st;
  pthread_mutex_lock (&vm->mod_mutex);
  for (i = 0; i < vm->nmodules; i++)
    if (strcmp (vm->modules[i]->name, module->name) == 0)
      {
        pthread_mutex_unlock (&vm->mod_mutex);
        ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "module '%s' already registered", module->name);
        return TS_ERR_INVAL;
      }
  if (vm->nmodules >= TS_MAX_MODULES)
    {
      pthread_mutex_unlock (&vm->mod_mutex);
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "module registry full");
      return TS_ERR_LIMIT;
    }
  vm->modules[vm->nmodules++] = module;
  pthread_mutex_unlock (&vm->mod_mutex);
  return TS_OK;
}

void
ts_vm_set_userdata (TS_VM *vm, void *userdata)
{
  if (vm)
    vm->userdata = userdata;
}

void *
ts_vm_get_userdata (const TS_VM *vm)
{
  return vm ? vm->userdata : NULL;
}

static void
ts_value_release (TS_Value *v)
{
  if (!v)
    return;
  if (v->type == TS_STRING)
    free (v->as.string);
  else if (v->type == TS_HANDLE)
    ts_handle_release ((TS_Handle *)v->as.handle);
  v->type = TS_NIL;
  v->as.handle = NULL;
}

TS_Status
ts_value_copy (TS_Value *dst, const TS_Value *src)
{
  if (!dst || !src)
    return TS_ERR_INVAL;
  if (dst == src)
    return TS_OK;
  dst->type = src->type;
  if (src->type == TS_STRING)
    {
      dst->as.string = src->as.string ? strdup (src->as.string) : NULL;
      if (src->as.string && !dst->as.string)
        return TS_ERR_NOMEM;
    }
  else if (src->type == TS_HANDLE)
    {
      dst->as.handle = src->as.handle;
      ts_handle_retain ((TS_Handle *)dst->as.handle);
    }
  else
    dst->as = src->as;
  return TS_OK;
}

void
ts_value_free (TS_Value *value)
{
  ts_value_release (value);
}

void
ts_value_make_nil (TS_Value *v)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_NIL;
}

void
ts_value_make_bool (TS_Value *v, bool b)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_BOOL;
  v->as.boolean = b;
}

void
ts_value_make_int (TS_Value *v, int64_t i)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_INT;
  v->as.integer = i;
}

TS_Status
ts_value_make_string (TS_Value *v, const char *s)
{
  char *copy;
  if (!v)
    return TS_ERR_INVAL;
  copy = strdup (s ? s : "");
  if (!copy)
    return TS_ERR_NOMEM;
  ts_value_release (v);
  v->type = TS_STRING;
  v->as.string = copy;
  return TS_OK;
}

TS_Status
ts_value_make_handle (TS_Value *v, void *ptr, TS_HandleFree free_fn,
                      const char *desc, const void *tag)
{
  TS_Handle *h;
  if (!v)
    return TS_ERR_INVAL;
  h = ts_handle_new (ptr, free_fn, tag, desc);
  if (!h)
    return TS_ERR_NOMEM;
  ts_value_release (v);
  v->type = TS_HANDLE;
  v->as.handle = h;
  return TS_OK;
}

void *
ts_value_handle (const TS_Value *v, const void *tag)
{
  TS_Handle *h;
  if (!v || v->type != TS_HANDLE || !(h = v->as.handle))
    return NULL;
  if (tag && h->tag != tag)
    return NULL;
  return h->ptr;
}

const char *
ts_handle_desc (const TS_Value *v)
{
  TS_Handle *h;
  if (!v || v->type != TS_HANDLE || !(h = v->as.handle))
    return "<handle>";
  return h->desc;
}

bool
ts_value_truthy (const TS_Value *v)
{
  switch (v->type)
    {
    case TS_NIL:
      return false;
    case TS_BOOL:
      return v->as.boolean;
    case TS_INT:
      return v->as.integer != 0;
    case TS_STRING:
      return v->as.string && v->as.string[0] != '\0';
    case TS_HANDLE:
      return true;
    }
  return false;
}

TS_Status
ts_value_to_string (const TS_Value *value, char **out, TS_Error *error)
{
  char tmp[64];
  const char *s = NULL;
  if (!value || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  switch (value->type)
    {
    case TS_NIL:
      s = "nil";
      break;
    case TS_BOOL:
      s = value->as.boolean ? "true" : "false";
      break;
    case TS_INT:
      snprintf (tmp, sizeof tmp, "%lld", (long long)value->as.integer);
      s = tmp;
      break;
    case TS_STRING:
      s = value->as.string ? value->as.string : "";
      break;
    case TS_HANDLE:
      s = value->as.handle ? ((TS_Handle *)value->as.handle)->desc
                           : "<handle>";
      break;
    }
  *out = strdup (s);
  if (!*out)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

/* ---------------- builtin modules ---------------- */

TS_Status
ts_check_argc (TS_VM *vm, const TS_Value *argv, size_t argc, size_t want,
               TS_Error *error)
{
  (void)vm;
  (void)argv;
  if (argc != want)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "want %zu args, got %zu", want,
                    argc);
      return TS_ERR_INVAL;
    }
  return TS_OK;
}

static bool
term_is_int (const TS_Value *v)
{
  return v->type == TS_INT;
}

static TS_Status
term_arith (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
            TS_Error *error, char op)
{
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (op == '+' && argv[0].type == TS_STRING && argv[1].type == TS_STRING)
    {
      size_t a = strlen (argv[0].as.string), b = strlen (argv[1].as.string);
      if (a > TS_MAX_FILE || b > TS_MAX_FILE - a)
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                        "string size limit exceeded");
          return TS_ERR_LIMIT;
        }
      char *s = malloc (a + b + 1);
      if (!s)
        return TS_ERR_NOMEM;
      memcpy (s, argv[0].as.string, a);
      memcpy (s + a, argv[1].as.string, b + 1);
      ts_value_free (ret);
      ret->type = TS_STRING;
      ret->as.string = s;
      return TS_OK;
    }
  if (!term_is_int (&argv[0]) || !term_is_int (&argv[1]))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "ints required");
      return TS_ERR_INVAL;
    }
  int64_t a = argv[0].as.integer, b = argv[1].as.integer, result = 0;
  bool overflow = false;
  switch (op)
    {
    case '+':
      overflow = (b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b);
      if (!overflow)
        result = a + b;
      break;
    case '-':
      overflow = (b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b);
      if (!overflow)
        result = a - b;
      break;
    case '*':
      if (a > 0)
        overflow = b > 0 ? a > INT64_MAX / b : b < INT64_MIN / a;
      else if (a < 0)
        overflow = b > 0 ? a < INT64_MIN / b : b < 0 && a < INT64_MAX / b;
      if (!overflow)
        result = a * b;
      break;
    case '/':
      if (argv[1].as.integer == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, 0, "division by zero");
          return TS_ERR_PARSE;
        }
      overflow = a == INT64_MIN && b == -1;
      if (!overflow)
        result = a / b;
      break;
    case 'm':
      if (argv[1].as.integer == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, 0, "modulo by zero");
          return TS_ERR_PARSE;
        }
      result = a == INT64_MIN && b == -1 ? 0 : a % b;
      break;
    default:
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad op");
      return TS_ERR_INVAL;
    }
  if (overflow)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "integer overflow");
      return TS_ERR_PARSE;
    }
  ts_value_make_int (ret, result);
  return TS_OK;
}

static TS_Status
g_add (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  return term_arith (v, a, n, r, e, '+');
}
static TS_Status
g_sub (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  return term_arith (v, a, n, r, e, '-');
}
static TS_Status
g_mul (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  return term_arith (v, a, n, r, e, '*');
}
static TS_Status
g_div (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  return term_arith (v, a, n, r, e, '/');
}
static TS_Status
g_mod (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  return term_arith (v, a, n, r, e, 'm');
}

static TS_Status
term_cmp (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
          TS_Error *error, char op)
{
  bool b = false;
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (op == 'e')
    {
      if (argv[0].type != argv[1].type)
        b = false;
      else if (argv[0].type == TS_INT)
        b = argv[0].as.integer == argv[1].as.integer;
      else if (argv[0].type == TS_BOOL)
        b = argv[0].as.boolean == argv[1].as.boolean;
      else if (argv[0].type == TS_STRING)
        b = strcmp (argv[0].as.string ? argv[0].as.string : "",
                    argv[1].as.string ? argv[1].as.string : "")
            == 0;
      else if (argv[0].type == TS_NIL)
        b = true;
      else
        b = argv[0].as.handle == argv[1].as.handle;
    }
  else
    {
      if (!term_is_int (&argv[0]) || !term_is_int (&argv[1]))
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0, "ints required");
          return TS_ERR_INVAL;
        }
      if (op == 'l')
        b = argv[0].as.integer < argv[1].as.integer;
      else if (op == 'g')
        b = argv[0].as.integer > argv[1].as.integer;
      else if (op == 'L')
        b = argv[0].as.integer <= argv[1].as.integer;
      else
        b = argv[0].as.integer >= argv[1].as.integer;
    }
  ret->type = TS_BOOL;
  ret->as.boolean = b;
  return TS_OK;
}

static TS_Status
g_eq (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void)ud;
  return term_cmp (v, a, n, r, e, 'e');
}
static TS_Status
g_lt (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void)ud;
  return term_cmp (v, a, n, r, e, 'l');
}
static TS_Status
g_gt (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void)ud;
  return term_cmp (v, a, n, r, e, 'g');
}
static TS_Status
g_le (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void)ud;
  return term_cmp (v, a, n, r, e, 'L');
}
static TS_Status
g_ge (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void)ud;
  return term_cmp (v, a, n, r, e, 'G');
}

static TS_Status
g_not (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
       TS_Error *error)
{
  (void)ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  ret->type = TS_BOOL;
  ret->as.boolean = !ts_value_truthy (&argv[0]);
  return TS_OK;
}

static TS_Status
g_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
       TS_Error *error)
{
  (void)ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "string required");
      return TS_ERR_INVAL;
    }
  ret->type = TS_INT;
  ret->as.integer
      = (int64_t)strlen (argv[0].as.string ? argv[0].as.string : "");
  return TS_OK;
}

static TS_Status
g_concat (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void)ud;
  char *a = NULL, *b = NULL;
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &a, error) != TS_OK)
    return TS_ERR_NOMEM;
  if (ts_value_to_string (&argv[1], &b, error) != TS_OK)
    {
      free (a);
      return TS_ERR_NOMEM;
    }
  ret->type = TS_STRING;
  ret->as.string = malloc (strlen (a) + strlen (b) + 1);
  if (!ret->as.string)
    {
      free (a);
      free (b);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  strcpy (ret->as.string, a);
  strcat (ret->as.string, b);
  free (a);
  free (b);
  return TS_OK;
}

static TS_Status
g_type (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error)
{
  (void)ud;
  const char *t = "nil";
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  switch (argv[0].type)
    {
    case TS_NIL:
      t = "nil";
      break;
    case TS_BOOL:
      t = "bool";
      break;
    case TS_INT:
      t = "int";
      break;
    case TS_STRING:
      t = "string";
      break;
    case TS_HANDLE:
      t = "handle";
      break;
    }
  ret->type = TS_STRING;
  ret->as.string = strdup (t);
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
g_load (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error)
{
  (void)ud;
  const TS_Module *mod;
  TS_Handle *h;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "module name must be a string");
      return TS_ERR_INVAL;
    }
  mod = ts_vm_lookup (vm, argv[0].as.string);
  if (!mod)
    {
      ret->type = TS_NIL; /* Load failure is nil (see `or`). */
      return TS_OK;
    }
  h = ts_handle_new_module (mod);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ret->type = TS_HANDLE;
  ret->as.handle = h;
  return TS_OK;
}

static TS_Status
g_puts (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error)
{
  (void)ud;
  char *s = NULL;
  size_t n;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &s, error) != TS_OK)
    return TS_ERR_NOMEM;
  n = strlen (s);
  if (vm->olen + n + 2 > vm->ocap)
    {
      size_t nc = vm->ocap ? vm->ocap * 2 : 256;
      char *nb;
      while (nc < vm->olen + n + 2)
        nc *= 2;
      nb = realloc (vm->output, nc);
      if (!nb)
        {
          free (s);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->output = nb;
      vm->ocap = nc;
    }
  memcpy (vm->output + vm->olen, s, n);
  vm->olen += n;
  vm->output[vm->olen++] = '\n';
  vm->output[vm->olen] = '\0';
  free (s);
  ret->type = TS_NIL;
  return TS_OK;
}

static TS_Status
g_die (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
       TS_Error *error)
{
  (void)ud;
  char *s = NULL;
  (void)vm;
  (void)ret;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &s, error) != TS_OK)
    s = NULL;
  ts_error_set (error, TS_ERR_CANCELLED, 0, 0, "%s", s ? s : "die");
  free (s);
  return TS_ERR_CANCELLED;
}

static TS_Status
g_import (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void)ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "import name must be a string");
      return TS_ERR_INVAL;
    }
  if (ts_vm_import (vm, argv[0].as.string, error) != TS_OK)
    return error ? error->code : TS_ERR_NOTFOUND;
  ret->type = TS_NIL;
  return TS_OK;
}

static TS_Status
g_import_path (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
               TS_Value *ret, TS_Error *error)
{
  (void)ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "path must be a string");
      return TS_ERR_INVAL;
    }
  if (ts_vm_add_search_path (vm, argv[0].as.string, error) != TS_OK)
    return error ? error->code : TS_ERR_INVAL;
  ret->type = TS_NIL;
  return TS_OK;
}

static TS_Status
g_native_import (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
                 TS_Value *ret, TS_Error *error)
{
  TS_Status st = g_load (vm, ud, argv, argc, ret, error);
  if (argc == 1 && argv[0].type == TS_STRING)
    snprintf (vm->last_libname, sizeof vm->last_libname, "%s",
              argv[0].as.string);
  if (st == TS_OK && ret->type == TS_NIL)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0,
                    "native module '%s' is not registered", argv[0].as.string);
      return TS_ERR_NOTFOUND;
    }
  return st;
}

static TS_Status
r_exit (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error)
{
  (void)ud;
  (void)ret;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  int64_t code;
  if (argv[0].type == TS_INT)
    code = argv[0].as.integer;
  else if (argv[0].type == TS_STRING
           && !strcmp (argv[0].as.string, "EXIT_SUCCESS"))
    code = 0;
  else if (argv[0].type == TS_STRING
           && !strcmp (argv[0].as.string, "EXIT_FAILURE"))
    code = 1;
  else
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "exit requires an integer or EXIT_SUCCESS/EXIT_FAILURE");
      return TS_ERR_INVAL;
    }
  vm->exit_requested = true;
  vm->exit_status = code;
  ts_error_set (error, TS_ERR_CANCELLED, 0, 0, "requested exit %lld",
                (long long)code);
  return TS_ERR_CANCELLED;
}

/* Error-reporting callbacks accept a format and optional printable values.
 * They observe the envelope; flow propagation retains the original error. */
static TS_Status
r_report (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void)ud;
  if (!argc || argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "report requires a format string");
      return TS_ERR_INVAL;
    }
  /* Formatting is deliberately bounded; no native printf format execution. */
  char text[TS_ERROR_MSG_SIZE];
  size_t len = 0, arg = 1;
  const char *p = argv[0].as.string;
  while (*p && len + 1 < sizeof text)
    {
      if ((*p == '~' || *p == '%') && p[1] && strchr ("sed", p[1])
          && arg < argc)
        {
          char *s = NULL;
          TS_Status st = ts_value_to_string (&argv[arg++], &s, error);
          if (st != TS_OK)
            return st;
          size_t n = strlen (s);
          if (n > sizeof text - len - 1)
            n = sizeof text - len - 1;
          memcpy (text + len, s, n);
          len += n;
          free (s);
          p += 2;
        }
      else
        text[len++] = *p++;
    }
  text[len] = '\0';
  TS_Value value = { 0 };
  TS_Status st = ts_value_make_string (&value, text);
  if (st == TS_OK)
    st = g_puts (vm, NULL, &value, 1, ret, error);
  ts_value_free (&value);
  return st;
}

static TS_Status r_inject (TS_VM *, void *, const TS_Value *, size_t,
                           TS_Value *, TS_Error *);

static const TS_FuncDef r_funcs[]
    = { { "Inject", r_inject, NULL },
        { "Exit", r_exit, NULL },
        { "DieGracefully", r_exit, NULL },
        { "CleanupAndExit", r_exit, NULL },
        { "Print", r_report, NULL },
        { "PrintError", r_report, NULL },
        { "ImportError", r_report, NULL },
        { "RuntimeError", r_report, NULL },
        { "IOError", r_report, NULL },
        { "ConfigurationError", r_report, NULL },
        { "ConnectionError", r_report, NULL },
        { "AuthenticationError", r_report, NULL },
        { "PermissionError", r_report, NULL },
        { "GrammarError", r_report, NULL },
        { "ParseError", r_report, NULL } };
static const TS_Module r_module = TS_MODULE_INIT ("R", r_funcs);

static TS_Status
ts_collection_call (TS_VM *vm, const TS_Module *module, const char *name,
                    const TS_Value *argv, size_t argc, TS_Value *ret,
                    TS_Error *error)
{
  const TS_FuncDef *f = ts_module_find_func (module, name);
  if (!f)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0,
                    "collection operation '%s:%s' is unavailable",
                    module ? module->name : "?", name);
      return TS_ERR_NOTFOUND;
    }
  TS_Status st = f->func (vm, f->userdata, argv, argc, ret, error);
  if (st != TS_OK)
    ts_value_free (ret);
  return st;
}

static TS_Status
g_collection (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error)
{
  bool map = ud != NULL;
  const char *name = map ? "std.map" : "std.list";
  const TS_Module *module = ts_vm_lookup (vm, name);
  if (!module)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0,
                    "collection literals require registered module '%s'",
                    name);
      return TS_ERR_NOTFOUND;
    }
  if (map && argc % 2)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "map requires key/value pairs");
      return TS_ERR_INVAL;
    }
  TS_Status st = ts_collection_call (vm, module, "new", NULL, 0, ret, error);
  for (size_t i = 0; st == TS_OK && i < argc; i += map ? 2 : 1)
    {
      TS_Value args[3] = { *ret, argv[i], { 0 } }, ignored = { 0 };
      if (map)
        args[2] = argv[i + 1];
      st = ts_collection_call (vm, module, map ? "set" : "push", args,
                               map ? 3 : 2, &ignored, error);
      ts_value_free (&ignored);
    }
  if (st != TS_OK)
    ts_value_free (ret);
  return st;
}
static const char ts_map_constructor;

TS_Status
ts_vm_set_args (TS_VM *vm, size_t argc, const char *const *argv,
                TS_Error *error)
{
  if (!vm || (argc && !argv) || argc > 4096)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "invalid script arguments");
      return TS_ERR_INVAL;
    }
  char **copy = calloc (argc ? argc : 1, sizeof *copy);
  if (!copy)
    return TS_ERR_NOMEM;
  for (size_t i = 0; i < argc; i++)
    {
      if (!argv[i] || !(copy[i] = strdup (argv[i])))
        {
          for (size_t j = 0; j < i; j++)
            free (copy[j]);
          free (copy);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                        "invalid argument or out of memory");
          return TS_ERR_NOMEM;
        }
    }
  for (size_t i = 0; i < vm->argc; i++)
    free (vm->argv[i]);
  free (vm->argv);
  vm->argv = copy;
  vm->argc = argc;
  return TS_OK;
}

int
ts_vm_exit_code (const TS_VM *vm)
{
  return vm ? vm->main_status : 1;
}

static TS_Status
g_args (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void)ud;
  if (ts_check_argc (vm, a, n, 0, e) != TS_OK)
    return TS_ERR_INVAL;
  TS_Value *values = calloc (vm->argc ? vm->argc : 1, sizeof *values);
  if (!values)
    return TS_ERR_NOMEM;
  for (size_t i = 0; i < vm->argc; i++)
    {
      values[i].type = TS_STRING;
      values[i].as.string = vm->argv[i];
    }
  TS_Status st = g_collection (vm, NULL, values, vm->argc, r, e);
  free (values);
  return st;
}

static TS_Status
g_entry (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void)ud;
  (void)r;
  if (ts_check_argc (vm, a, n, 1, e) != TS_OK)
    return TS_ERR_INVAL;
  if (a[0].type == TS_NIL)
    {
      vm->main_status = 0;
      return TS_OK;
    }
  if (a[0].type != TS_INT || a[0].as.integer < 0 || a[0].as.integer > 255)
    {
      ts_error_set (e, TS_ERR_INVAL, 0, 0,
                    "main must return nil or an exit code in [0, 255]");
      return TS_ERR_INVAL;
    }
  vm->main_status = (int)a[0].as.integer;
  return TS_OK;
}

static TS_Status
g_integer_op (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
              TS_Error *e)
{
  intptr_t op = (intptr_t)ud;
  if (ts_check_argc (vm, a, n, op == 5 ? 1 : 2, e) != TS_OK)
    return TS_ERR_INVAL;
  for (size_t i = 0; i < n; i++)
    if (a[i].type != TS_INT)
      {
        ts_error_set (e, TS_ERR_INVAL, 0, 0, "integer operands required");
        return TS_ERR_INVAL;
      }
  int64_t x = a[0].as.integer, y = n > 1 ? a[1].as.integer : 0, result;
  switch (op)
    {
    case 0:
      result = x & y;
      break;
    case 1:
      result = x | y;
      break;
    case 2:
      result = x ^ y;
      break;
    case 3:
    case 4:
      if (y < 0 || y >= 64)
        {
          ts_error_set (e, TS_ERR_INVAL, 0, 0,
                        "shift count must be in [0, 63]");
          return TS_ERR_INVAL;
        }
      /* Bit-pattern shifts: left wraps, right is logical, never signed UB. */
      {
        uint64_t bits = op == 3 ? (uint64_t)x << y : (uint64_t)x >> y;
        memcpy (&result, &bits, sizeof result);
      }
      break;
    case 5:
      if (x == INT64_MIN)
        {
          ts_error_set (e, TS_ERR_PARSE, 0, 0, "integer overflow");
          return TS_ERR_PARSE;
        }
      result = -x;
      break;
    case 6:
      result = x < y ? x : y;
      break;
    default:
      result = x > y ? x : y;
      break;
    }
  ts_value_make_int (r, result);
  return TS_OK;
}

static TS_Status
g_ne (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  TS_Status st = g_eq (vm, ud, a, n, r, e);
  if (st == TS_OK)
    r->as.boolean = !r->as.boolean;
  return st;
}

static const TS_FuncDef g_funcs[]
    = { { "args", g_args, NULL },
        { "__entry", g_entry, NULL },
        { "band", g_integer_op, (void *)0 },
        { "bor", g_integer_op, (void *)1 },
        { "bxor", g_integer_op, (void *)2 },
        { "shl", g_integer_op, (void *)3 },
        { "shr", g_integer_op, (void *)4 },
        { "neg", g_integer_op, (void *)5 },
        { "min", g_integer_op, (void *)6 },
        { "max", g_integer_op, (void *)7 },
        { "ne", g_ne, NULL },
        { "error", g_die, NULL },
        { "list", g_collection, NULL },
        { "map", g_collection, (void *)&ts_map_constructor },
        { "Import", g_native_import, NULL },
        { "load", g_load, NULL },
        { "puts", g_puts, NULL },
        { "die", g_die, NULL },
        { "import", g_import, NULL },
        { "import_path", g_import_path, NULL },
        { "add", g_add, NULL },
        { "sub", g_sub, NULL },
        { "mul", g_mul, NULL },
        { "div", g_div, NULL },
        { "mod", g_mod, NULL },
        { "eq", g_eq, NULL },
        { "lt", g_lt, NULL },
        { "gt", g_gt, NULL },
        { "le", g_le, NULL },
        { "ge", g_ge, NULL },
        { "not", g_not, NULL },
        { "len", g_len, NULL },
        { "concat", g_concat, NULL },
        { "type", g_type, NULL } };

static const TS_Module g_module = TS_MODULE_INIT ("G", g_funcs);

/* ---------------- VM construction helpers ---------------- */

/* ---------------- AST ---------------- */

typedef enum
{
  N_SEQ,
  N_CONST,
  N_ASSIGN,
  N_EXPR_STMT,
  N_OR,
  N_VAR,
  N_CALL,
  N_LIT_INT,
  N_LIT_STR,
  N_LIT_BOOL,
  N_LIT_NIL,
  N_WHILE,
  N_IF,
  N_ALIAS,
  N_DEFINE,
  N_INTERFACE,
  N_SECTION,
  N_MANAGED,
  N_BLOCK,
  N_FLOW,
  N_FIELD,
  N_NOT,
  N_AND,
  N_FUNCTION,
  N_RETURN
} node_kind_t;

typedef struct dt_arg
{
  bool is_ref;
  char *name; /* For IDENT / &IDENT. */
  int64_t number;
  char *string;
  int tag; /* 0=name, 1=number, 2=string, 3=bool, 4=nil. */
  bool boolean;
  struct dt_node *expr; /* tag 5: owned general expression. */
} ts_arg_t;

typedef struct dt_node
{
  node_kind_t kind;
  unsigned line;
  struct dt_node *next;      /* For sequences. */
  struct dt_node *first;     /* Body / sequence head. */
  struct dt_node *cond;      /* while/if condition. */
  struct dt_node *otherwise; /* if-else branch head. */
  struct dt_node *lhs;       /* or-branches. */
  struct dt_node *rhs;
  char *name;   /* const/assign/var. */
  char *module; /* call. */
  char *func;   /* call. */
  ts_arg_t *args;
  size_t nargs;
  int64_t number;
  char *string;
  bool boolean;
} ts_node_t;

/* Track front-end scratch separately from the returned AST. Backtracking
 * abandons tokens and partial trees; a completed parse retains only reachable
 * AST allocations. The allocator context is thread-local so AST disposal on
 * another VM never touches the active parser's scratch storage. */
enum
{
  TS_PARSE_BUCKETS = 16384
};
typedef struct ts_parse_allocation
{
  void *pointer;
  bool retained;
  struct ts_parse_allocation *next;
} ts_parse_allocation;
typedef struct
{
  ts_parse_allocation *buckets[TS_PARSE_BUCKETS];
} ts_parse_arena;
static _Thread_local ts_parse_arena *ts_parse_memory;
void ts_oom (void);

static ts_parse_allocation **
ts_parse_slot (uintptr_t key)
{
  size_t bucket = ((key >> 4) ^ (key >> 18)) & (TS_PARSE_BUCKETS - 1);
  ts_parse_allocation **slot = &ts_parse_memory->buckets[bucket];
  while (*slot && (uintptr_t)(*slot)->pointer != key)
    slot = &(*slot)->next;
  return slot;
}

void *
ts_parse_alloc (size_t size)
{
  void *p = malloc (size);
  ts_parse_allocation *entry;
  if (!ts_parse_memory)
    return p;
  if (!p)
    {
      ts_oom ();
      return NULL;
    }
  entry = malloc (sizeof *entry);
  if (!entry)
    {
      free (p);
      ts_oom ();
      return NULL;
    }
  ts_parse_allocation **slot = ts_parse_slot ((uintptr_t)p);
  entry->pointer = p;
  entry->retained = false;
  entry->next = NULL;
  *slot = entry;
  return p;
}

void
ts_parse_free (void *p)
{
  if (p && ts_parse_memory)
    {
      ts_parse_allocation **slot = ts_parse_slot ((uintptr_t)p),
                          *entry = *slot;
      if (entry)
        {
          *slot = entry->next;
          free (entry);
        }
    }
  free (p);
}

static void *
ts_parse_calloc (size_t count, size_t size)
{
  if (!ts_parse_memory)
    return calloc (count, size);
  if (count && size > SIZE_MAX / count)
    {
      ts_oom ();
      return NULL;
    }
  void *p = ts_parse_alloc (count * size);
  if (p)
    memset (p, 0, count * size);
  return p;
}

static void *
ts_parse_realloc (void *p, size_t size)
{
  if (!ts_parse_memory)
    return realloc (p, size);
  if (!p)
    return ts_parse_alloc (size);
  if (!size)
    {
      ts_parse_free (p);
      return NULL;
    }
  ts_parse_allocation **slot = ts_parse_slot ((uintptr_t)p), *entry = *slot;
  void *next = realloc (p, size);
  if (!next)
    {
      ts_oom ();
      return NULL;
    }
  if (entry)
    {
      *slot = entry->next;
      entry->pointer = next;
      slot = ts_parse_slot ((uintptr_t)next);
      entry->next = NULL;
      *slot = entry;
    }
  return next;
}

static bool
ts_parse_keep (const void *p)
{
  if (!p)
    return false;
  ts_parse_allocation *entry = *ts_parse_slot ((uintptr_t)p);
  if (!entry || entry->retained)
    return false;
  entry->retained = true;
  return true;
}

static void
ts_parse_keep_tree (const ts_node_t *n)
{
  for (; n; n = n->next)
    {
      if (!ts_parse_keep (n))
        continue;
      ts_parse_keep (n->name);
      ts_parse_keep (n->module);
      ts_parse_keep (n->func);
      ts_parse_keep (n->string);
      ts_parse_keep (n->args);
      for (size_t i = 0; i < n->nargs; i++)
        {
          ts_parse_keep (n->args[i].name);
          ts_parse_keep (n->args[i].string);
          ts_parse_keep_tree (n->args[i].expr);
        }
      ts_parse_keep_tree (n->first);
      ts_parse_keep_tree (n->cond);
      ts_parse_keep_tree (n->otherwise);
      ts_parse_keep_tree (n->lhs);
      if (n->kind != N_SEQ)
        ts_parse_keep_tree (n->rhs);
    }
}

/* Only the front end uses these hooks; published AST storage is ordinary
 * heap storage and continues to use the existing ownership/destructor API. */
#define malloc ts_parse_alloc
#define calloc ts_parse_calloc
#define realloc ts_parse_realloc
#define free ts_parse_free

static void
ts_node_free (ts_node_t *n)
{
  size_t i;
  while (n)
    {
      ts_node_t *nx = n->next;
      if (n->kind == N_SEQ)
        {
          /* Sequence wrapper from ts_mk_seq/ts_seq_append: first owns the
           * statement chain, rhs is a non-owning tail link into it. */
          ts_node_t *ch = n->first;
          free (n);
          n = ch;
          continue;
        }
      ts_node_free (n->first);
      ts_node_free (n->cond);
      ts_node_free (n->otherwise);
      ts_node_free (n->lhs);
      ts_node_free (n->rhs);
      free (n->name);
      free (n->module);
      free (n->func);
      for (i = 0; i < n->nargs; i++)
        {
          free (n->args[i].name);
          free (n->args[i].string);
          ts_node_free (n->args[i].expr);
        }
      free (n->args);
      free (n->string);
      free (n);
      n = nx;
    }
}

static ts_node_t *
ts_node_new (node_kind_t kind, unsigned line)
{
  ts_node_t *n = calloc (1, sizeof *n);
  if (n)
    {
      n->kind = kind;
      n->line = line;
    }
  return n;
}

/* ---------------- NovoParse front end ---------------- */
/*
 * Termscript is parsed by the NovoParse descriptor generated from
 * termscript/Termscript.ng.  The adapter in ts_parser_ng.c walks the syntax
 * tree and calls the ts_* constructors below, which build the same ts_node_t
 * AST the evaluator walks.  Ownership rules:
 *
 * - Every ts_* helper either adopts each `char *` it is given or frees
 *   it before returning; callers never touch adopted/freed buffers.
 * - Helpers return NULL only on allocation failure (recorded with
 *   ts_oom()) or deliberate rule rejection (ts_check_or on a non-`or`
 *   word, ts_mk_argword/ts_mk_unary on the reserved word `or`, and the
 *   %empty untail/if_tail markers).  Grammar actions turn helper NULLs
 *   into `return 0` (backtrack), except the two intentional-NULL slots
 *   consumed by ts_mk_unary (plain variable) and ts_mk_if (missing
 *   else), which succeed.
 * - A top-level NULL result with ts_oom() set means TS_ERR_NOMEM;
 *   without it, a syntax error (with a best-effort offset).
 * - Line numbers come from ts_curline(attempt.s): the byte offset is
 *   rewound over trailing layout before counting newlines, so lines are
 *   exact unless trailing `#` comment lines intervene (documented
 *   approximation, off by the comment count).
 * - Parse calls are serialized by ts_parse_mutex because the per-parse input
 *   window and allocation-failure flag live in ts_pctx.  Evaluation itself
 *   stays per-VM and lock-free.
 */

void *ts_ng_parse (const char *input); /* ts_parser_ng.c */
size_t ts_ng_error_offset (void);
const char *ts_ng_error_message (void);

/* ts_mk_string/ts_mk_number are defined below. */
void *ts_mk_string (char *tok, unsigned line);
void *ts_mk_number (char *tok, unsigned line);

static pthread_mutex_t ts_parse_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct
{
  const char *start;
  const char *end;
  bool oom;
} ts_pctx = { NULL, NULL, false };

void
ts_oom (void)
{
  ts_pctx.oom = true;
}

unsigned
ts_curline (const char *pos)
{
  const char *start = ts_pctx.start;
  const char *end = ts_pctx.end;
  const char *p = pos;
  const char *q;
  unsigned line = 1;
  if (!start || !end)
    return 1;
  if (p < start)
    p = start;
  if (p > end)
    p = end;
  while (p > start
         && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n'))
    p--;
  for (q = start; q < p; q++)
    if (*q == '\n')
      line++;
  return line;
}

void *
ts_kw (char *tok, const char **pos, unsigned kwlen)
{
  /* Keyword-with-boundary match (e.g. "const " or "end" at EOF): return
   * the single boundary character to the input so the rest of the
   * grammar sees it.  Losing it would corrupt parsing (`const=x` would
   * misparse with the `=` silently eaten).  The boundary is exactly one
   * character, or empty at end of input (kwlen match, `$` branch). */
  size_t len;
  if (!tok || !pos || !*pos)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  len = strlen (tok);
  if (len < kwlen)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  *pos -= len - kwlen;
  free (tok);
  return (void *)1;
}

void *
ts_check_or (char *tok)
{
  bool is_or;
  if (!tok)
    {
      ts_oom ();
      return NULL;
    }
  is_or = strcmp (tok, "or") == 0;
  free (tok);
  return is_or ? (void *)1 : NULL;
}

static bool ts_reserved (const char *tok);

char *
ts_ident (char *tok)
{
  if (!tok)
    {
      ts_oom ();
      return NULL;
    }
  if (ts_reserved (tok))
    {
      free (tok); /* Reserved word where a name is required. */
      return NULL;
    }
  return tok; /* Adopted as the identifier text. */
}

static ts_node_t *
ts_newnode (node_kind_t kind, unsigned line)
{
  ts_node_t *n = ts_node_new (kind, line);
  if (!n)
    ts_oom ();
  return n;
}

void *
ts_mk_seq (void)
{
  /* N_SEQ wrapper: first = statement chain head, rhs = tail link
   * (non-owning; the tail is owned through the chain).  ts_node_free
   * knows rhs must not be freed twice. */
  return ts_newnode (N_SEQ, 1);
}

void *
ts_seq_append (void *seq, void *stmt)
{
  ts_node_t *s = (ts_node_t *)seq;
  ts_node_t *st = (ts_node_t *)stmt;
  if (!s || !st || s->kind != N_SEQ)
    return NULL; /* OOM upstream already recorded. */
  if (st->kind == N_SEQ)
    {
      ts_node_t *head = st->first, *tail = st->rhs;
      st->first = st->rhs = NULL;
      free (st);
      if (!head)
        return s;
      if (s->first)
        s->rhs->next = head;
      else
        s->first = head;
      s->rhs = tail;
      return s;
    }
  if (!s->first)
    s->first = st;
  else
    s->rhs->next = st;
  s->rhs = st;
  return s;
}

/* Argument accumulator (private to the front end). */
typedef struct
{
  ts_arg_t *v;
  size_t n;
  size_t cap;
} ts_args_t;

static void
ts_arg_free (ts_arg_t *a)
{
  if (!a)
    return;
  free (a->name);
  free (a->string);
  ts_node_free (a->expr);
  free (a);
}

static void
ts_args_free_all (ts_args_t *ac)
{
  size_t i;
  if (!ac)
    return;
  for (i = 0; i < ac->n; i++)
    {
      free (ac->v[i].name);
      free (ac->v[i].string);
      ts_node_free (ac->v[i].expr);
    }
  free (ac->v);
  free (ac);
}

void *
ts_args_new (void)
{
  ts_args_t *ac = calloc (1, sizeof *ac);
  if (!ac)
    {
      ts_oom ();
      return NULL;
    }
  ac->v = calloc (4, sizeof *ac->v);
  if (!ac->v)
    {
      free (ac);
      ts_oom ();
      return NULL;
    }
  ac->cap = 4;
  return ac;
}

void *
ts_args_add (void *args, void *arg)
{
  ts_args_t *ac = (ts_args_t *)args;
  ts_arg_t *item = (ts_arg_t *)arg;
  ts_arg_t *nv;
  if (!ac || !item)
    {
      ts_arg_free (item);
      if (ac)
        ts_args_free_all (ac);
      return NULL;
    }
  if (ac->n == ac->cap)
    {
      size_t nc = ac->cap * 2;
      nv = realloc (ac->v, nc * sizeof *nv);
      if (!nv)
        {
          ts_arg_free (item);
          ts_args_free_all (ac);
          ts_oom ();
          return NULL;
        }
      ac->v = nv;
      ac->cap = nc;
    }
  ac->v[ac->n++] = *item;
  free (item);
  return ac;
}

static ts_arg_t *
ts_arg_new (unsigned line)
{
  ts_arg_t *a = calloc (1, sizeof *a);
  (void)line;
  if (!a)
    ts_oom ();
  return a;
}

void *
ts_mk_argstr (char *tok, unsigned line)
{
  ts_arg_t *a;
  ts_node_t *lit;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  /* Reuse literal string unescaping, then move the decoded text over. */
  lit = (ts_node_t *)ts_mk_string (tok, line);
  if (!lit)
    {
      free (a);
      return NULL;
    }
  a->tag = 2;
  a->string = lit->string;
  lit->string = NULL;
  ts_node_free (lit);
  return a;
}

void *
ts_mk_argnum (char *tok, unsigned line)
{
  ts_arg_t *a;
  ts_node_t *lit;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  lit = (ts_node_t *)ts_mk_number (tok, line);
  if (!lit)
    {
      free (a);
      return NULL;
    }
  a->tag = 1;
  a->number = lit->number;
  ts_node_free (lit);
  return a;
}

/* Shared word dispatch for ts_mk_unary (tail == NULL), ts_mk_argword
 * and ts_ident: true/false/nil become literals, reserved words are
 * rejected (the old hand lexer produced distinct keyword tokens, so
 * none of these could ever be a reference).  `or`/`do`/`end`/`else`
 * must fail in argument position or greedy call arguments would swallow
 * the operator/body delimiters that follow a call.  Returns 1 and fills
 * *kind_out for a literal, 0 for a plain name, -1 for reserved. */
static bool
ts_reserved (const char *tok)
{
  static const char *const words[]
      = { "or",  "do",    "then",   "end",      "else", "while",
          "if",  "const", "config", "lifetime", "not",  "and",
          "let", "var",   "fn",     "return",   "use",  NULL };
  size_t i;
  for (i = 0; words[i]; i++)
    if (strcmp (tok, words[i]) == 0)
      return true;
  return false;
}

static int
ts_word_lit (const char *tok, node_kind_t *kind_out, bool *boolean_out)
{
  if (strcmp (tok, "true") == 0)
    {
      *kind_out = N_LIT_BOOL;
      *boolean_out = true;
      return 1;
    }
  if (strcmp (tok, "false") == 0)
    {
      *kind_out = N_LIT_BOOL;
      *boolean_out = false;
      return 1;
    }
  if (strcmp (tok, "nil") == 0)
    {
      *kind_out = N_LIT_NIL;
      *boolean_out = false;
      return 1;
    }
  if (ts_reserved (tok))
    return -1;
  return 0;
}

void *
ts_mk_argword (char *tok, unsigned line)
{
  ts_arg_t *a;
  node_kind_t kind;
  bool boolean = false;
  int disp;
  if (!tok)
    return NULL;
  disp = ts_word_lit (tok, &kind, &boolean);
  if (disp < 0)
    {
      free (tok); /* Reserved `or`: fail the arg, backtrack. */
      return NULL;
    }
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  if (disp > 0)
    {
      free (tok);
      if (kind == N_LIT_NIL)
        a->tag = 4;
      else
        {
          a->tag = 3;
          a->boolean = boolean;
        }
      return a;
    }
  a->tag = 0;
  a->name = tok;
  return a;
}

void *
ts_mk_argref (char *tok, unsigned line)
{
  ts_arg_t *a;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  a->tag = 0;
  a->is_ref = true;
  a->name = tok;
  return a;
}

void *
ts_mk_number (char *tok, unsigned line)
{
  ts_node_t *n;
  char *end = NULL;
  long long v;
  if (!tok)
    return NULL;
  errno = 0;
  const char *digits = tok[0] == '-' ? tok + 1 : tok;
  v = strtoll (
      tok, &end,
      digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X') ? 16 : 10);
  if (errno == ERANGE || !end || *end != '\0')
    {
      free (tok); /* Malformed/out-of-range: fail the rule. */
      return NULL;
    }
  n = ts_newnode (N_LIT_INT, line);
  free (tok);
  if (!n)
    return NULL;
  n->number = (int64_t)v;
  return n;
}

void *
ts_mk_string (char *tok, unsigned line)
{
  ts_node_t *n;
  size_t len;
  char *out;
  size_t i, o;
  if (!tok)
    return NULL;
  len = strlen (tok);
  if (len < 2 || tok[0] != '"' || tok[len - 1] != '"')
    {
      free (tok);
      return NULL;
    }
  out = malloc (len); /* Decoded form is strictly shorter. */
  if (!out)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  /* Same escape set as the historical hand lexer: n t r " \ 0. */
  for (i = 1, o = 0; i + 1 < len;)
    {
      char c = tok[i++];
      if (c != '\\')
        {
          out[o++] = c;
          continue;
        }
      if (i >= len)
        break;
      switch (tok[i++])
        {
        case 'n':
          out[o++] = '\n';
          break;
        case 't':
          out[o++] = '\t';
          break;
        case 'r':
          out[o++] = '\r';
          break;
        case '"':
          out[o++] = '"';
          break;
        case '\\':
          out[o++] = '\\';
          break;
        case 'e':
          out[o++] = '\033';
          break;
        case 'a':
          out[o++] = '\a';
          break;
        case 'b':
          out[o++] = '\b';
          break;
        case 'f':
          out[o++] = '\f';
          break;
        case 'v':
          out[o++] = '\v';
          break;
        case 'x':
          {
            unsigned value = 0, j;
            for (j = 0; j < 2; j++)
              {
                unsigned char d = (unsigned char)tok[i++];
                int digit = d >= '0' && d <= '9'   ? d - '0'
                            : d >= 'a' && d <= 'f' ? d - 'a' + 10
                            : d >= 'A' && d <= 'F' ? d - 'A' + 10
                                                   : -1;
                if (i > len - 1 || digit < 0)
                  {
                    free (out);
                    free (tok);
                    return NULL;
                  }
                value = value * 16 + (unsigned)digit;
              }
            if (!value)
              {
                free (out);
                free (tok);
                return NULL;
              }
            out[o++] = (char)value;
            break;
          }
        case '0':
          free (out);
          free (tok);
          return NULL; /* Strings cannot hold NUL. */
        default:
          free (out);
          free (tok);
          return NULL; /* Bad escape: fail the rule. */
        }
    }
  out[o] = '\0';
  n = ts_newnode (N_LIT_STR, line);
  free (tok);
  if (!n)
    {
      free (out);
      return NULL;
    }
  n->string = out;
  return n;
}

void *
ts_mk_var (char *name, unsigned line)
{
  ts_node_t *n;
  if (!name)
    return NULL;
  n = ts_newnode (N_VAR, line);
  if (!n)
    {
      free (name);
      return NULL;
    }
  n->name = name;
  return n;
}

void *
ts_mk_ref (char *name, unsigned line)
{
  /* `&name` is an explicit-reference sigil with identical value
   * semantics to a plain reference; kept as N_VAR. */
  return ts_mk_var (name, line);
}

void *
ts_mk_callrest (char *func, void *args, unsigned line)
{
  ts_args_t *ac = (ts_args_t *)args;
  ts_node_t *n;
  if (!func || !ac)
    {
      free (func);
      if (ac)
        ts_args_free_all (ac);
      return NULL;
    }
  n = ts_newnode (N_CALL, line);
  if (!n)
    {
      free (func);
      ts_args_free_all (ac);
      return NULL;
    }
  n->func = func; /* module filled in by ts_mk_unary. */
  n->args = ac->v;
  n->nargs = ac->n;
  free (ac);
  return n;
}

void *
ts_mk_unary (char *name, void *tail, unsigned line)
{
  ts_node_t *call = (ts_node_t *)tail;
  if (!name)
    {
      if (call)
        ts_node_free (call);
      return NULL;
    }
  if (!call)
    {
      node_kind_t kind;
      bool boolean = false;
      int disp = ts_word_lit (name, &kind, &boolean);
      if (disp < 0)
        {
          free (name); /* Reserved `or`: fail the rule. */
          return NULL;
        }
      if (disp > 0)
        {
          ts_node_t *n = ts_newnode (kind, line);
          free (name);
          if (!n)
            return NULL;
          if (kind == N_LIT_BOOL)
            n->boolean = boolean;
          return n;
        }
      return ts_mk_var (name, line);
    }
  call->module = name;
  call->line = line;
  return call;
}

static void *
ts_mk_stmt2 (node_kind_t kind, const char *name, void *rhs, unsigned line)
{
  ts_node_t *n = ts_newnode (kind, line);
  if (!n)
    {
      free ((void *)name);
      ts_node_free ((ts_node_t *)rhs);
      return NULL;
    }
  n->name = (char *)name;
  n->rhs = (ts_node_t *)rhs;
  if (!n->name || !n->rhs)
    {
      /* Partial build can only happen on OOM, already recorded. */
      ts_node_free (n);
      return NULL;
    }
  return n;
}

void *
ts_mk_const (char *name, void *expr, unsigned line)
{
  return ts_mk_stmt2 (N_CONST, name, expr, line);
}

void *
ts_mk_assign (char *name, void *expr, unsigned line)
{
  return ts_mk_stmt2 (N_ASSIGN, name, expr, line);
}

void *
ts_mk_exprstmt (void *expr, unsigned line)
{
  ts_node_t *n = ts_newnode (N_EXPR_STMT, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *)expr);
      return NULL;
    }
  n->rhs = (ts_node_t *)expr;
  if (!n->rhs)
    {
      ts_node_free (n);
      return NULL;
    }
  return n;
}

void *
ts_mk_while (void *cond, void *body, unsigned line)
{
  ts_node_t *n = ts_newnode (N_WHILE, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *)cond);
      ts_node_free ((ts_node_t *)body);
      return NULL;
    }
  n->cond = (ts_node_t *)cond;
  {
    ts_node_t *b = (ts_node_t *)body;
    if (!n->cond || !b)
      {
        ts_node_free (b);
        ts_node_free (n);
        return NULL;
      }
    /* Unwrap the N_SEQ body wrapper, keeping its statement chain. */
    n->first = b->first;
    free (b);
  }
  return n;
}

void *
ts_mk_if (void *cond, void *thenb, void *elseb, unsigned line)
{
  ts_node_t *n = ts_newnode (N_IF, line);
  ts_node_t *then_seq = (ts_node_t *)thenb;
  ts_node_t *else_seq = (ts_node_t *)elseb;
  if (!n)
    {
      ts_node_free ((ts_node_t *)cond);
      ts_node_free (then_seq);
      ts_node_free (else_seq);
      return NULL;
    }
  n->cond = (ts_node_t *)cond;
  if (!n->cond || !then_seq)
    {
      ts_node_free (then_seq);
      ts_node_free (else_seq);
      ts_node_free (n);
      return NULL;
    }
  /* Unwrap the N_SEQ branch wrappers, keeping their chains. */
  n->first = then_seq->first;
  n->otherwise = else_seq ? else_seq->first : NULL;
  free (then_seq);
  free (else_seq);
  return n;
}

void *
ts_mk_or (void *lhs, void *rhs, unsigned line)
{
  ts_node_t *n = ts_newnode (N_OR, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *)lhs);
      ts_node_free ((ts_node_t *)rhs);
      return NULL;
    }
  n->lhs = (ts_node_t *)lhs;
  n->rhs = (ts_node_t *)rhs;
  if (!n->lhs || !n->rhs)
    {
      ts_node_free (n);
      return NULL;
    }
  return n;
}

/* Parse one input under the front-end lock.  Returns the N_SEQ program
 * wrapper, or NULL (oom set on allocation failure). */
static void *
ts_try_parse (const char *input, bool *oom)
{
  void *r;
  pthread_mutex_lock (&ts_parse_mutex);
  ts_parse_arena *arena = calloc (1, sizeof *arena);
  if (!arena)
    {
      if (oom)
        *oom = true;
      pthread_mutex_unlock (&ts_parse_mutex);
      return NULL;
    }
  ts_parse_memory = arena;
  ts_pctx.start = input;
  ts_pctx.end = input + strlen (input);
  ts_pctx.oom = false;
  r = ts_ng_parse (input);
  if (ts_pctx.oom)
    r = NULL;
  if (r)
    ts_parse_keep_tree (r);
  ts_parse_memory = NULL;
  for (size_t i = 0; i < TS_PARSE_BUCKETS; i++)
    {
      ts_parse_allocation *entry = arena->buckets[i];
      while (entry)
        {
          ts_parse_allocation *next = entry->next;
          if (!entry->retained)
            free (entry->pointer);
          free (entry);
          entry = next;
        }
    }
  free (arena);
  if (oom)
    *oom = ts_pctx.oom;
  ts_pctx.start = NULL;
  ts_pctx.end = NULL;
  pthread_mutex_unlock (&ts_parse_mutex);
  return r;
}

#include "sections_front.inc"

void *
ts_mk_function (char *name, void *arguments, void *body, unsigned line)
{
  ts_args_t *args = arguments;
  ts_node_t *seq = body, *n = ts_newnode (N_FUNCTION, line);
  if (!n || !args || !seq)
    return NULL;
  n->name = name;
  n->args = args->v;
  n->nargs = args->n;
  free (args);
  n->first = seq->first;
  seq->first = seq->rhs = NULL;
  ts_node_free (seq);
  return n;
}

void *
ts_mk_return (void *expr, unsigned line)
{
  ts_node_t *n = ts_newnode (N_RETURN, line);
  if (n)
    n->rhs = expr;
  return n;
}

void *
ts_mk_and (void *lhs, void *rhs, unsigned line)
{
  ts_node_t *n = ts_mk_or (lhs, rhs, line);
  if (n)
    n->kind = N_AND;
  return n;
}

/* A bounded source-loading preflight, not a token stream for the parser.
 * Strings/comments do not contribute structural nesting. NovoParse remains
 * the sole syntax parser; malformed delimiters are diagnosed there. */
static TS_Status
ts_source_limits (const char *source, TS_Error *error)
{
  size_t size = strnlen (source, TS_MAX_SOURCE_SIZE);
  if (size == TS_MAX_SOURCE_SIZE)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, size,
                    "Termscript source exceeds 1 MiB");
      return TS_ERR_LIMIT;
    }
  unsigned delimiters = 0, groups = 0, blocks = 0, operators = 0;
  const char *p = source;
  char previous = 0;
  while (*p)
    {
      if (isspace ((unsigned char)*p))
        {
          if (*p == '\n' && !groups && !strchr ("+-*/%&|^<>=~!", previous))
            {
              const char *next = p + 1;
              for (;;)
                {
                  while (isspace ((unsigned char)*next))
                    next++;
                  if (*next != '#')
                    break;
                  while (*next && *next != '\n')
                    next++;
                }
              if (!*next || !strchr ("+-*/%&|^<>=~!", *next))
                operators = 0;
            }
          p++;
          continue;
        }
      if (*p == '#')
        {
          while (*p && *p != '\n')
            p++;
          continue;
        }
      if (*p == '"')
        {
          p++;
          while (*p && *p != '"')
            {
              if (*p == '\\' && p[1])
                p += 2;
              else
                p++;
            }
          if (*p)
            p++;
          previous = '"';
          continue;
        }
      if (strchr ("([{", *p))
        delimiters++;
      else if (strchr (")]}", *p) && delimiters)
        delimiters--;
      else if (*p == ';' || *p == '}')
        operators = 0;
      if (*p == '(' || *p == '[')
        groups++;
      else if ((*p == ')' || *p == ']') && groups)
        groups--;
      if (isalpha ((unsigned char)*p) || *p == '_')
        {
          const char *start = p++;
          while (isalnum ((unsigned char)*p) || *p == '_')
            p++;
          size_t n = (size_t)(p - start);
          if (previous != ':' && previous != '$' && previous != '>')
            {
              if ((n == 2 && !memcmp (start, "do", 2))
                  || (n == 4 && !memcmp (start, "then", 4))
                  || (previous == '.'
                      && ((n == 7 && !memcmp (start, "section", 7))
                          || (n == 9 && !memcmp (start, "interface", 9)))))
                blocks++;
              else if (n == 3 && !memcmp (start, "end", 3) && blocks)
                blocks--;
              else if (n == 2 && !memcmp (start, "or", 2))
                operators++;
            }
          previous = 'a';
        }
      else
        {
          if (strchr ("+-*/%&|^<>=~!", *p))
            operators++;
          previous = *p++;
        }
      if (delimiters + blocks > TS_MAX_SYNTAX_NESTING
          || operators > TS_MAX_EXPR_OPERATORS)
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, (size_t)(p - source),
                        "Termscript source nesting/operator limit exceeded");
          return TS_ERR_LIMIT;
        }
    }
  return TS_OK;
}

static TS_Status
ts_validate_functions (ts_node_t *list, unsigned depth, bool function,
                       TS_Error *error)
{
  for (ts_node_t *n = list; n; n = n->next)
    {
      bool fn = n->kind == N_FUNCTION;
      if ((fn && depth) || (n->kind == N_RETURN && !function))
        {
          ts_error_set (error, TS_ERR_INVAL, 0, n->line,
                        fn ? "nested functions/closures are not supported"
                           : "return outside a function");
          return TS_ERR_INVAL;
        }
      if (fn)
        {
          for (ts_node_t *other = n->next; other; other = other->next)
            if (other->kind == N_FUNCTION && !strcmp (n->name, other->name))
              {
                ts_error_set (error, TS_ERR_INVAL, 0, n->line,
                              "duplicate function '%s'", n->name);
                return TS_ERR_INVAL;
              }
          for (size_t i = 0; i < n->nargs; i++)
            for (size_t j = 0; j < i; j++)
              if (!strcmp (n->args[i].name, n->args[j].name))
                {
                  ts_error_set (error, TS_ERR_INVAL, 0, n->line,
                                "duplicate parameter '%s'", n->args[i].name);
                  return TS_ERR_INVAL;
                }
        }
      TS_Status st
          = ts_validate_functions (n->first, depth + 1, function || fn, error);
      if (st == TS_OK)
        st = ts_validate_functions (n->otherwise, depth + 1, function, error);
      if (st != TS_OK)
        return st;
    }
  return TS_OK;
}

static TS_Status
ts_main_entry (ts_node_t *program, TS_Error *error)
{
  ts_node_t *main = NULL, *tail = program->first;
  for (ts_node_t *n = tail; n; n = n->next)
    if (n->kind == N_FUNCTION && !strcmp (n->name, "main"))
      main = n;
  if (!main)
    return TS_OK;
  if (main->nargs > 1)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, main->line,
                    "main accepts zero parameters or one argument list");
      return TS_ERR_INVAL;
    }
  void *args = ts_args_new ();
  if (main->nargs)
    {
      void *argv = ts_mk_unary (
          ts_front_dup ("G"),
          ts_mk_callrest (ts_front_dup ("args"), ts_args_new (), main->line),
          main->line);
      args = ts_args_add (args, ts_mk_argexpr (argv));
    }
  void *call = ts_mk_unary (
      ts_front_dup ("main"),
      ts_mk_callrest (ts_front_dup ("__call"), args, main->line), main->line);
  void *entry = ts_mk_unary (
      ts_front_dup ("G"),
      ts_mk_callrest (ts_front_dup ("__entry"),
                      ts_args_add (ts_args_new (), ts_mk_argexpr (call)),
                      main->line),
      main->line);
  ts_node_t *stmt = ts_mk_exprstmt (entry, main->line);
  if (!stmt)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  while (tail->next)
    tail = tail->next;
  tail->next = stmt;
  return TS_OK;
}

static TS_Status
ts_parse_program (const char *source, ts_node_t **out, TS_Error *error)
{
  *out = NULL;
  TS_Status limit = ts_source_limits (source, error);
  if (limit != TS_OK)
    return limit;
  bool oom = false;
  void *r = ts_try_parse (source, &oom);
  if (r)
    {
      *out = (ts_node_t *)r;
      TS_Status st = ts_validate_functions ((*out)->first, 0, false, error);
      if (st == TS_OK)
        st = ts_lower_sections (*out, error);
      if (st == TS_OK)
        st = ts_main_entry (*out, error);
      if (st == TS_ERR_NOMEM)
        ts_error_set (error, st, ENOMEM, 0, "out of memory lowering sections");
      if (st != TS_OK)
        {
          ts_node_free (*out);
          *out = NULL;
        }
      return st;
    }
  if (oom)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  *out = NULL;
  ts_error_set (error, TS_ERR_PARSE, 0, ts_ng_error_offset (), "%s",
                ts_ng_error_message ());
  return TS_ERR_PARSE;
}
#undef malloc
#undef calloc
#undef realloc
#undef free
/* ---------------- interpreter ---------------- */

static ts_var_t *
vm_lookup (TS_VM *vm, const char *name)
{
  ts_var_t *v;
  for (v = vm->vars; v && (!vm->frame || v != vm->frame->base); v = v->next)
    if (strcmp (v->name, name) == 0)
      return v;
  if (vm->frame)
    for (v = vm->frame->globals; v; v = v->next)
      if (!strcmp (v->name, name))
        return v;
  return NULL;
}

static TS_Status
vm_step_budget (TS_VM *vm, TS_Error *error)
{
  if (++vm->steps > TS_MAX_STEPS)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "termscript step budget hit");
      return TS_ERR_LIMIT;
    }
  return TS_OK;
}

static TS_Status eval_node (TS_VM *vm, ts_node_t *n, TS_Value *out,
                            TS_Error *error);
static TS_Status exec_list (TS_VM *vm, ts_node_t *list, TS_Error *error);
static TS_Status eval_flow (TS_VM *, ts_node_t *, TS_Value *, TS_Error *);
static TS_Status ts_jit_eval_expr (TS_VM *, ts_node_t *, TS_Value *,
                                   TS_Error *);
static TS_Status vm_define (TS_VM *, const char *, TS_Value *, bool, unsigned,
                            TS_Error *);

typedef struct
{
  TS_Func callback;
  void *data;
  void (*destroy) (void *);
} ts_callable;
static const char ts_callable_tag;

static void
ts_callable_free (void *ptr)
{
  ts_callable *c = ptr;
  if (c->destroy)
    c->destroy (c->data);
  free (c);
}

static TS_Status
ts_define_callable (TS_VM *vm, const char *name, TS_Func callback, void *data,
                    void (*destroy) (void *), TS_Error *error)
{
  ts_callable *c = calloc (1, sizeof *c);
  if (!c)
    {
      if (destroy)
        destroy (data);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  c->callback = callback;
  c->data = data;
  c->destroy = destroy;
  TS_Value v = { 0 };
  TS_Status st
      = ts_value_make_handle (&v, c, ts_callable_free, name, &ts_callable_tag);
  if (st != TS_OK)
    ts_callable_free (c);
  else
    st = vm_define (vm, name, &v, true, 0, error);
  ts_value_free (&v);
  if (st == TS_ERR_NOMEM)
    ts_error_set (error, st, ENOMEM, 0, "out of memory");
  return st;
}

TS_Status
ts_rt_function (TS_VM *vm, const char *name, TS_Func callback, TS_Error *error)
{
  if (!vm || !name || !callback)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "invalid function definition");
      return TS_ERR_INVAL;
    }
  return ts_define_callable (vm, name, callback, NULL, NULL, error);
}

TS_Status
ts_rt_param (TS_VM *vm, const char *name, const TS_Value *value,
             TS_Error *error)
{
  if (!vm || !name || !value || !vm->frame)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "parameter outside a function");
      return TS_ERR_INVAL;
    }
  TS_Value copy = { 0 };
  TS_Status st = ts_value_copy (&copy, value);
  if (st == TS_OK)
    st = vm_define (vm, name, &copy, false, 0, error);
  ts_value_free (&copy);
  return st;
}

static void
ts_function_tree_free (void *ptr)
{
  ts_node_free (ptr);
}

static TS_Status
ts_function_eval (TS_VM *vm, void *data, const TS_Value *args, size_t argc,
                  TS_Value *ret, TS_Error *error)
{
  ts_node_t *fn = data;
  TS_Status st = ts_check_argc (vm, args, argc, fn->nargs, error);
  for (size_t i = 0; st == TS_OK && i < argc; i++)
    st = ts_rt_param (vm, fn->args[i].name, &args[i], error);
  if (st == TS_OK)
    st = exec_list (vm, fn->first, error);
  if (st == TS_OK && vm->frame->returning)
    {
      *ret = vm->frame->result;
      vm->frame->result.type = TS_NIL;
    }
  return st;
}

/* Shared call dispatcher: invoke `func` on the module carried by the
 * handle value `modval` (borrowed).  Used by the tree-walking
 * interpreter and by ts_rt_call for ahead-of-time compiled C. */
static TS_Status
ts_call_with_handle (TS_VM *vm, const TS_Value *modval, const char *modname,
                     const char *func, const TS_Value *argv, size_t argc,
                     TS_Value *ret, unsigned line, TS_Error *error)
{
  TS_Error local_error;
  if (!error)
    {
      ts_error_clear (&local_error);
      error = &local_error;
    }
  TS_Handle *h;
  const TS_FuncDef *f = NULL;
  const TS_Module *mod = NULL;
  TS_Status st;
  ts_callable *callable = ts_value_handle (modval, &ts_callable_tag);
  if (callable && func && !strcmp (func, "__call"))
    {
      if (vm->call_depth >= TS_MAX_SYNTAX_NESTING)
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, line,
                        "function recursion limit exceeded");
          return TS_ERR_LIMIT;
        }
      ts_frame frame = { .base = vm->vars,
                         .managed = vm->managed,
                         .globals = vm->frame ? vm->frame->globals : vm->vars,
                         .previous = vm->frame };
      vm->frame = &frame;
      vm->call_depth++;
      st = callable->callback (vm, callable->data, argv, argc, ret, error);
      if (st != TS_OK && error->code == TS_OK)
        ts_error_set (error, st, 0, line, "function '%s' failed: %s",
                      modname ? modname : "?", ts_status_string (st));
      while (vm->vars != frame.base)
        {
          ts_var_t *v = vm->vars;
          vm->vars = v->next;
          ts_value_free (&v->value);
          free (v->name);
          free (v);
        }
      vm->managed = frame.managed;
      vm->frame = frame.previous;
      vm->call_depth--;
      ts_value_free (&frame.result);
      if (st != TS_OK)
        ts_value_free (ret);
      return st;
    }
  if (!modval || modval->type != TS_HANDLE || !(h = modval->as.handle)
      || !(mod = ts_handle_module (h)))
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line,
                    "module '%s' is not bound", modname ? modname : "?");
      return TS_ERR_NOTFOUND;
    }
  f = ts_module_find_func (mod, func);
  if (!f)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line,
                    "no such function '%s:%s'", modname ? modname : "?",
                    func ? func : "?");
      return TS_ERR_NOTFOUND;
    }
  ret->type = TS_NIL;
  snprintf (vm->last_libname, sizeof vm->last_libname, "%s", mod->name);
  st = f->func (vm, f->userdata, argv, argc, ret, error);
  if (st != TS_OK)
    {
      ts_value_free (ret);
      if (error && error->code == TS_OK)
        ts_error_set (error, st, 0, line, "native call '%s:%s' failed: %s",
                      modname, func, ts_status_string (st));
    }
  return st;
}

static TS_Status
eval_call (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  ts_var_t *mv = vm_lookup (vm, n->module);
  TS_Value *argv = NULL;
  TS_Value modval;
  TS_Status st;
  size_t i;
  modval.type = TS_NIL;
  if (mv)
    {
      if (ts_value_copy (&modval, &mv->value) != TS_OK)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  if (n->nargs)
    {
      argv = calloc (n->nargs, sizeof *argv);
      if (!argv)
        {
          ts_value_release (&modval);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      for (i = 0; i < n->nargs; i++)
        {
          ts_arg_t *a = &n->args[i];
          if (a->tag == 5)
            {
              st = eval_node (vm, a->expr, &argv[i], error);
              if (st != TS_OK)
                {
                  for (size_t k = 0; k <= i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  return st;
                }
            }
          else if (a->tag == 0)
            {
              st = ts_rt_get (vm, a->name, &argv[i], n->line, error);
              if (st != TS_OK)
                {
                  size_t k;
                  for (k = 0; k < i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  return st;
                }
            }
          else if (a->tag == 1)
            {
              argv[i].type = TS_INT;
              argv[i].as.integer = a->number;
            }
          else if (a->tag == 2)
            {
              argv[i].type = TS_STRING;
              argv[i].as.string = strdup (a->string);
              if (!argv[i].as.string)
                {
                  size_t k;
                  for (k = 0; k < i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                                "out of memory");
                  return TS_ERR_NOMEM;
                }
            }
          else if (a->tag == 3)
            {
              argv[i].type = TS_BOOL;
              argv[i].as.boolean = a->boolean;
            }
          else
            argv[i].type = TS_NIL;
        }
    }
  st = ts_call_with_handle (vm, mv ? &modval : NULL, n->module, n->func, argv,
                            n->nargs, out, n->line, error);
  ts_value_release (&modval);
  if (argv)
    {
      for (i = 0; i < n->nargs; i++)
        ts_value_release (&argv[i]);
      free (argv);
    }
  return st;
}

static TS_Status
eval_node (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  TS_Status st;
  if (!n)
    {
      out->type = TS_NIL;
      return TS_OK;
    }
  if (vm_step_budget (vm, error) != TS_OK)
    return TS_ERR_LIMIT;
  switch (n->kind)
    {
    case N_VAR:
      {
        if (!strcmp (n->name, "$E"))
          return ts_rt_get (vm, n->name, out, n->line, error);
        ts_var_t *v = vm_lookup (vm, n->name);
        if (!v)
          {
            ts_error_set (error, TS_ERR_NOTFOUND, 0, n->line,
                          "unbound variable '%s'", n->name);
            return TS_ERR_NOTFOUND;
          }
        return ts_value_copy (out, &v->value) == TS_OK ? TS_OK : TS_ERR_NOMEM;
      }
    case N_LIT_INT:
      out->type = TS_INT;
      out->as.integer = n->number;
      return TS_OK;
    case N_LIT_STR:
      out->type = TS_STRING;
      out->as.string = strdup (n->string);
      if (!out->as.string)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      return TS_OK;
    case N_LIT_BOOL:
      out->type = TS_BOOL;
      out->as.boolean = n->boolean;
      return TS_OK;
    case N_LIT_NIL:
      out->type = TS_NIL;
      return TS_OK;
    case N_CALL:
      return vm->jit ? ts_jit_eval_expr (vm, n, out, error)
                     : eval_call (vm, n, out, error);
    case N_BLOCK:
      out->type = TS_NIL;
      return exec_list (vm, n->first, error);
    case N_FIELD:
      return ts_rt_field (vm, n->name, n->func, out, n->line, error);
    case N_FLOW:
      return eval_flow (vm, n, out, error);
    case N_NOT:
      {
        TS_Value v = { 0 };
        st = eval_node (vm, n->lhs, &v, error);
        if (st == TS_OK)
          ts_value_make_bool (out, !ts_value_truthy (&v));
        ts_value_free (&v);
        return st;
      }
    case N_AND:
    case N_OR:
      {
        TS_Value lv;
        lv.type = TS_NIL;
        st = eval_node (vm, n->lhs, &lv, error);
        if (st != TS_OK)
          return st;
        if (ts_value_truthy (&lv) == (n->kind == N_OR))
          {
            *out = lv;
            return TS_OK;
          }
        ts_value_release (&lv);
        return eval_node (vm, n->rhs, out, error);
      }
    default:
      ts_error_set (error, TS_ERR_PARSE, 0, n->line, "not an expression");
      return TS_ERR_PARSE;
    }
}

static TS_Status
vm_define (TS_VM *vm, const char *name, TS_Value *val, bool is_const,
           unsigned line, TS_Error *error)
{
  if (!strcmp (name, "$E"))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line,
                    "$E is a reserved flow envelope");
      return TS_ERR_INVAL;
    }
  ts_var_t *v = vm_lookup (vm, name);
  if (vm->frame)
    {
      v = vm->vars;
      while (v != vm->frame->base && strcmp (v->name, name))
        v = v->next;
      if (v == vm->frame->base)
        v = NULL;
    }
  if (v)
    {
      if (v->is_const)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, line,
                        "cannot reassign const '%s'", name);
          return TS_ERR_INVAL;
        }
      ts_value_release (&v->value);
      v->value = *val;
      val->type = TS_NIL;
      val->as.handle = NULL;
      v->is_const = is_const;
      return TS_OK;
    }
  v = calloc (1, sizeof *v);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v->name = strdup (name);
  if (!v->name)
    {
      free (v);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v->value = *val;
  val->type = TS_NIL;
  val->as.handle = NULL;
  v->is_const = is_const;
  v->next = vm->vars;
  vm->vars = v;
  return TS_OK;
}

static TS_Status
r_inject (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret,
          TS_Error *error)
{
  (void)ud;
  (void)ret;
  if (ts_check_argc (vm, a, n, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (a[0].type != TS_HANDLE || a[1].type != TS_STRING || !a[1].as.string[0])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "Inject requires a handle and a symbol name");
      return TS_ERR_INVAL;
    }
  ts_var_t *existing = vm_lookup (vm, a[1].as.string);
  if (existing && existing->value.type == TS_HANDLE && a[0].type == TS_HANDLE
      && existing->value.as.handle == a[0].as.handle)
    return TS_OK;
  TS_Value value = { 0 };
  TS_Status st = ts_value_copy (&value, &a[0]);
  if (st == TS_OK)
    st = ts_rt_lifetime_set (vm, a[1].as.string, &value, 0, error);
  if (st == TS_OK)
    vm_lookup (vm, a[1].as.string)->injected = true;
  ts_value_free (&value);
  return st;
}

#include "sections_runtime.inc"

static TS_Status
exec_list (TS_VM *vm, ts_node_t *list, TS_Error *error)
{
  ts_node_t *s;
  for (s = list; s && !(vm->frame && vm->frame->returning); s = s->next)
    {
      TS_Status st;
      if (vm_step_budget (vm, error) != TS_OK)
        return TS_ERR_LIMIT;
      switch (s->kind)
        {
        case N_FUNCTION:
          {
            ts_node_t single = *s;
            single.next = NULL;
            ts_node_t *copy = ts_front_clone (&single);
            if (!copy)
              return TS_ERR_NOMEM;
            st = ts_define_callable (vm, s->name, ts_function_eval, copy,
                                     ts_function_tree_free, error);
            if (st != TS_OK)
              return st;
          }
          break;
        case N_RETURN:
          if (!vm->frame)
            {
              ts_error_set (error, TS_ERR_INVAL, 0, s->line,
                            "return outside a function");
              return TS_ERR_INVAL;
            }
          st = eval_node (vm, s->rhs, &vm->frame->result, error);
          if (st != TS_OK)
            return st;
          vm->frame->returning = true;
          break;
        case N_CONST:
        case N_ASSIGN:
        case N_MANAGED:
          {
            TS_Value v;
            v.type = TS_NIL;
            st = eval_node (vm, s->rhs, &v, error);
            if (st != TS_OK)
              return st;
            st = s->kind == N_MANAGED
                     ? ts_rt_lifetime_set (vm, s->name, &v, s->line, error)
                     : vm_define (vm, s->name, &v, s->kind == N_CONST, s->line,
                                  error);
            ts_value_release (&v);
            if (st != TS_OK)
              return st;
          }
          break;
        case N_EXPR_STMT:
          {
            TS_Value v;
            v.type = TS_NIL;
            st = eval_node (vm, s->rhs, &v, error);
            ts_value_release (&v);
            if (st != TS_OK)
              return st;
          }
          break;
        case N_WHILE:
          for (;;)
            {
              TS_Value c;
              bool go;
              c.type = TS_NIL;
              st = eval_node (vm, s->cond, &c, error);
              if (st != TS_OK)
                return st;
              go = ts_value_truthy (&c);
              ts_value_release (&c);
              if (!go)
                break;
              st = exec_list (vm, s->first, error);
              if (st != TS_OK)
                return st;
              if (vm->frame && vm->frame->returning)
                break;
            }
          break;
        case N_IF:
          {
            TS_Value c;
            bool go;
            c.type = TS_NIL;
            st = eval_node (vm, s->cond, &c, error);
            if (st != TS_OK)
              return st;
            go = ts_value_truthy (&c);
            ts_value_release (&c);
            st = exec_list (vm, go ? s->first : s->otherwise, error);
            if (st != TS_OK)
              return st;
          }
          break;
        default:
          ts_error_set (error, TS_ERR_PARSE, 0, s->line, "bad statement");
          return TS_ERR_PARSE;
        }
    }
  return TS_OK;
}

/* ---------------- compiled-code runtime ---------------- */

TS_Status
ts_rt_call (TS_VM *vm, const TS_Value *mod, const char *modname,
            const char *func, const TS_Value *argv, size_t argc, TS_Value *ret,
            unsigned line, TS_Error *error)
{
  if (!vm || !mod || !func || !ret || (argc && !argv))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad call");
      return TS_ERR_INVAL;
    }
  return ts_call_with_handle (vm, mod, modname, func, argv, argc, ret, line,
                              error);
}

TS_Status
ts_rt_get (TS_VM *vm, const char *name, TS_Value *out, unsigned line,
           TS_Error *error)
{
  ts_var_t *v;
  if (!vm || !name || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad arguments");
      return TS_ERR_INVAL;
    }
  if (!strcmp (name, "$E"))
    return ts_envelope_value (vm, out, line, error);
  v = vm_lookup (vm, name);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line, "unbound variable '%s'",
                    name);
      return TS_ERR_NOTFOUND;
    }
  if (ts_value_copy (out, &v->value) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

TS_Status
ts_rt_set (TS_VM *vm, const char *name, TS_Value *val, bool is_const,
           unsigned line, TS_Error *error)
{
  if (!vm || !name || !val)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad arguments");
      return TS_ERR_INVAL;
    }
  return vm_define (vm, name, val, is_const, line, error);
}

TS_Status
ts_rt_step (TS_VM *vm, TS_Error *error)
{
  if (!vm)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad vm");
      return TS_ERR_INVAL;
    }
  return vm_step_budget (vm, error);
}

const char *
ts_rt_output (const TS_VM *vm)
{
  if (!vm || !vm->output)
    return "";
  return vm->output;
}

/* ---------------- VM lifetime ---------------- */

static TS_Status
vm_bind_core (TS_VM *vm, TS_Error *error)
{
  const TS_Module *g = ts_vm_lookup (vm, "G");
  TS_Handle *h;
  TS_Value v;
  if (!g)
    {
      ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "G module missing");
      return TS_ERR_SYSTEM;
    }
  h = ts_handle_new_module (g);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v.type = TS_HANDLE;
  v.as.handle = h;
  TS_Status st = vm_define (vm, "G", &v, true, 0, error);
  ts_value_free (&v);
  if (st != TS_OK)
    return st;
  h = ts_handle_new_module (&r_module);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v.type = TS_HANDLE;
  v.as.handle = h;
  st = vm_define (vm, "R", &v, true, 0, error);
  ts_value_free (&v);
  return st;
}

TS_VM *
ts_vm_create (TS_Error *error)
{
  TS_VM *vm = calloc (1, sizeof *vm);
  TS_Status st;
  if (!vm)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  if (pthread_mutex_init (&vm->mod_mutex, NULL) != 0)
    {
      free (vm);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  st = ts_vm_register_module (vm, &g_module, error);
  if (st != TS_OK)
    {
      ts_vm_free (vm);
      return NULL;
    }
  if (vm_bind_core (vm, error) != TS_OK)
    {
      ts_vm_free (vm);
      return NULL;
    }
  return vm;
}

void
ts_vm_free (TS_VM *vm)
{
  ts_var_t *v;
  if (!vm)
    return;
  ts_rt_finish (vm, TS_OK, NULL);
  v = vm->vars;
  while (v)
    {
      ts_var_t *nx = v->next;
      free (v->name);
      ts_value_release (&v->value);
      free (v);
      v = nx;
    }
  free (vm->output);
  free (vm->compiled);
  for (size_t i = 0; i < vm->argc; i++)
    free (vm->argv[i]);
  free (vm->argv);
  {
    size_t i;
    for (i = 0; i < vm->nspaths; i++)
      free (vm->spaths[i]);
    free (vm->spaths);
    for (i = 0; i < vm->nistack; i++)
      free (vm->istack[i]);
    free (vm->istack);
  }
  pthread_mutex_destroy (&vm->mod_mutex);
  free (vm);
}

static void
vm_reset_run (TS_VM *vm)
{
  vm->steps = 0;
  vm->main_status = 0;
  vm->exit_requested = false;
  vm->exit_status = 0;
  vm->olen = 0;
  if (vm->output)
    vm->output[0] = '\0';
}

/* Parse `source` and execute it.  When `reset` is true the per-run
 * state (step budget, output accumulator) is cleared first, as for
 * ts_vm_run_string; imports pass false so variables, output and the
 * step budget are shared with the importing program. */
static TS_Status
ts_exec_source (TS_VM *vm, const char *source, bool reset, TS_Error *error)
{
  ts_node_t *prog;
  TS_Status st;
  st = ts_parse_program (source, &prog, error);
  if (st != TS_OK)
    return st;
  if (reset)
    vm_reset_run (vm);
  st = exec_list (vm, prog->first, error);
  if (reset)
    st = ts_rt_finish (vm, st, error);
  ts_node_free (prog);
  return st;
}

TS_Status
ts_vm_run_string (TS_VM *vm, const char *source, char **output,
                  TS_Error *error)
{
  TS_Status st;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  st = ts_exec_source (vm, source, true, error);
  if (st != TS_OK)
    return st;
  if (output)
    {
      *output = strdup (vm->output ? vm->output : "");
      if (!*output)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  return TS_OK;
}

TS_Status
ts_vm_run_file (TS_VM *vm, const char *path, char **output, TS_Error *error)
{
  FILE *f;
  char *buf = NULL;
  size_t cap = 4096, len = 0, r;
  TS_Status st;
  if (!vm || !path)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  f = fopen (path, "rb");
  if (!f)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "cannot open '%s'", path);
      return TS_ERR_SYSTEM;
    }
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  while ((r = fread (buf + len, 1, cap - len - 1, f)) > 0)
    {
      len += r;
      if (len + 1 >= cap)
        {
          char *nb;
          if (cap >= TS_MAX_FILE)
            {
              free (buf);
              fclose (f);
              ts_error_set (error, TS_ERR_LIMIT, 0, 0, "script too large");
              return TS_ERR_LIMIT;
            }
          cap *= 2;
          nb = realloc (buf, cap);
          if (!nb)
            {
              free (buf);
              fclose (f);
              ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          buf = nb;
        }
    }
  fclose (f);
  buf[len] = '\0';
  st = ts_vm_run_string (vm, buf, output, error);
  free (buf);
  return st;
}

enum
{
  TS_JIT_ADD = 0,
  TS_JIT_SUB,
  TS_JIT_MUL,
  TS_JIT_DIV,
  TS_JIT_MOD,
  TS_JIT_EQ,
  TS_JIT_LT,
  TS_JIT_GT,
  TS_JIT_LE,
  TS_JIT_GE
};

#if defined(TERMSCRIPT_HAVE_JITERATI)
static TS_Status
ts_jit_value_arg (TS_VM *vm, const ts_arg_t *arg, TS_Value *out,
                  TS_Error *error)
{
  if (!arg || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad Jiterati argument");
      return TS_ERR_INVAL;
    }
  if (arg->tag == 1)
    {
      ts_value_make_int (out, arg->number);
      return TS_OK;
    }
  else if (arg->tag == 5)
    return eval_node (vm, arg->expr, out, error);
  else if (arg->tag == 0)
    return ts_rt_get (vm, arg->name, out, 0, error);
  else if (arg->tag == 2)
    return ts_value_make_string (out, arg->string);
  else if (arg->tag == 3)
    ts_value_make_bool (out, arg->boolean);
  return TS_OK;
}

static int
ts_jit_g_opcode (const char *func, bool *is_bool)
{
  if (is_bool)
    *is_bool = false;
  if (!func)
    return -1;
  if (strcmp (func, "add") == 0)
    return TS_JIT_ADD;
  if (strcmp (func, "sub") == 0)
    return TS_JIT_SUB;
  if (strcmp (func, "mul") == 0)
    return TS_JIT_MUL;
  if (strcmp (func, "div") == 0)
    return TS_JIT_DIV;
  if (strcmp (func, "mod") == 0)
    return TS_JIT_MOD;
  if (is_bool)
    *is_bool = true;
  if (strcmp (func, "eq") == 0)
    return TS_JIT_EQ;
  if (strcmp (func, "lt") == 0)
    return TS_JIT_LT;
  if (strcmp (func, "gt") == 0)
    return TS_JIT_GT;
  if (strcmp (func, "le") == 0)
    return TS_JIT_LE;
  if (strcmp (func, "ge") == 0)
    return TS_JIT_GE;
  if (is_bool)
    *is_bool = false;
  return -1;
}

static TS_Status
ts_jit_checked_i32_binary (int opcode, int32_t lhs, int32_t rhs, unsigned line,
                           int32_t *out, TS_Error *error)
{
  int64_t r = 0;
  switch (opcode)
    {
    case TS_JIT_ADD:
      r = (int64_t)lhs + (int64_t)rhs;
      break;
    case TS_JIT_SUB:
      r = (int64_t)lhs - (int64_t)rhs;
      break;
    case TS_JIT_MUL:
      r = (int64_t)lhs * (int64_t)rhs;
      break;
    case TS_JIT_DIV:
      if (rhs == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, line, "division by zero");
          return TS_ERR_PARSE;
        }
      if (lhs == INT32_MIN && rhs == -1)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, line, "integer overflow");
          return TS_ERR_PARSE;
        }
      r = lhs / rhs;
      break;
    case TS_JIT_MOD:
      if (rhs == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, line, "modulo by zero");
          return TS_ERR_PARSE;
        }
      r = lhs == INT32_MIN && rhs == -1 ? 0 : lhs % rhs;
      break;
    case TS_JIT_EQ:
      r = lhs == rhs;
      break;
    case TS_JIT_LT:
      r = lhs < rhs;
      break;
    case TS_JIT_GT:
      r = lhs > rhs;
      break;
    case TS_JIT_LE:
      r = lhs <= rhs;
      break;
    case TS_JIT_GE:
      r = lhs >= rhs;
      break;
    default:
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, line,
                    "unknown Jiterati i32 opcode");
      return TS_ERR_UNSUPPORTED;
    }
  if (r < INT32_MIN || r > INT32_MAX)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, line, "integer overflow");
      return TS_ERR_PARSE;
    }
  *out = (int32_t)r;
  return TS_OK;
}
#endif

static TS_Status
ts_jit_eval_simple (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  if (!n || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad Jiterati expression");
      return TS_ERR_INVAL;
    }
  switch (n->kind)
    {
    case N_VAR:
      {
        ts_var_t *var = vm_lookup (vm, n->name);
        if (!var)
          {
            ts_error_set (error, TS_ERR_NOTFOUND, 0, n->line,
                          "unbound variable '%s'", n->name);
            return TS_ERR_NOTFOUND;
          }
        if (ts_value_copy (out, &var->value) != TS_OK)
          {
            ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
        return TS_OK;
      }
    case N_LIT_INT:
      out->type = TS_INT;
      out->as.integer = n->number;
      return TS_OK;
    case N_LIT_STR:
      out->type = TS_STRING;
      out->as.string = strdup (n->string);
      if (!out->as.string)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      return TS_OK;
    case N_LIT_BOOL:
      out->type = TS_BOOL;
      out->as.boolean = n->boolean;
      return TS_OK;
    case N_LIT_NIL:
      out->type = TS_NIL;
      return TS_OK;
    default:
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, n->line,
                    "Jiterati backend supports only simple values and G:add");
      return TS_ERR_UNSUPPORTED;
    }
}

static TS_Status
ts_jit_eval_expr (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  if (n && n->kind == N_OR)
    {
      TS_Value lhs;
      TS_Status st;
      lhs.type = TS_NIL;
      st = ts_jit_eval_expr (vm, n->lhs, &lhs, error);
      if (st != TS_OK)
        return st;
      if (ts_value_truthy (&lhs))
        {
          *out = lhs;
          return TS_OK;
        }
      ts_value_release (&lhs);
      return ts_jit_eval_expr (vm, n->rhs, out, error);
    }
  if (n && n->kind == N_CALL)
    {
#if defined(TERMSCRIPT_HAVE_JITERATI)
      int opcode;
      bool result_bool = false;
      int32_t lhs, rhs, checked, result;
      char detail[160];
      TS_Status st;
      ts_var_t *module = vm_lookup (vm, n->module);
      if (!module || module->value.type != TS_HANDLE
          || ts_handle_module (module->value.as.handle) != &g_module)
        return eval_call (vm, n, out, error);
      opcode = strcmp (n->module, "G") == 0
                   ? ts_jit_g_opcode (n->func, &result_bool)
                   : -1;
      if (opcode < 0)
        return eval_call (vm, n, out, error);
      if (n->nargs != 2)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, n->line,
                        "want 2 args, got %zu", n->nargs);
          return TS_ERR_INVAL;
        }
      TS_Value values[2] = { { 0 }, { 0 } };
      st = ts_jit_value_arg (vm, &n->args[0], &values[0], error);
      if (st == TS_OK)
        st = ts_jit_value_arg (vm, &n->args[1], &values[1], error);
      if (st != TS_OK)
        {
          ts_value_free (&values[0]);
          ts_value_free (&values[1]);
          return st;
        }
      bool i32 = values[0].type == TS_INT && values[1].type == TS_INT
                 && values[0].as.integer >= INT32_MIN
                 && values[0].as.integer <= INT32_MAX
                 && values[1].as.integer >= INT32_MIN
                 && values[1].as.integer <= INT32_MAX;
      if (!i32)
        {
          st = ts_call_with_handle (vm, &module->value, n->module, n->func,
                                    values, 2, out, n->line, error);
          ts_value_free (&values[0]);
          ts_value_free (&values[1]);
          return st;
        }
      lhs = (int32_t)values[0].as.integer;
      rhs = (int32_t)values[1].as.integer;
      ts_value_free (&values[0]);
      ts_value_free (&values[1]);
      st = ts_jit_checked_i32_binary (opcode, lhs, rhs, n->line, &checked,
                                      error);
      if (st != TS_OK)
        {
          /* Widening arithmetic is ordinary int64 Termscript, not i32
           * overflow. */
          TS_Value args[2] = { { .type = TS_INT }, { .type = TS_INT } };
          args[0].as.integer = lhs;
          args[1].as.integer = rhs;
          ts_error_clear (error);
          return ts_call_with_handle (vm, &module->value, n->module, n->func,
                                      args, 2, out, n->line, error);
        }
      detail[0] = '\0';
      if (ts_jiterati_binary_i32 (opcode, lhs, rhs, &result, detail,
                                  sizeof detail)
          != 0)
        {
          ts_error_set (error, TS_ERR_UNSUPPORTED, 0, n->line,
                        "Jiterati execution failed: %s",
                        detail[0] ? detail : "unknown error");
          return TS_ERR_UNSUPPORTED;
        }
      if (result != checked)
        {
          ts_error_set (error, TS_ERR_SYSTEM, 0, n->line,
                        "Jiterati result mismatch");
          return TS_ERR_SYSTEM;
        }
      if (result_bool)
        {
          out->type = TS_BOOL;
          out->as.boolean = result != 0;
        }
      else
        {
          out->type = TS_INT;
          out->as.integer = result;
        }
      return TS_OK;
#else
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, n->line,
                    "Jiterati backend was not built");
      return TS_ERR_UNSUPPORTED;
#endif
    }
  return ts_jit_eval_simple (vm, n, out, error);
}

static TS_Status
ts_vm_run_jiterati (TS_VM *vm, const char *source, char **output,
                    TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Status st;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  if (!ts_backend_available (TS_BACKEND_JITERATI))
    {
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "Jiterati backend is unavailable in this build");
      return TS_ERR_UNSUPPORTED;
    }
  st = ts_parse_program (source, &prog, error);
  if (st != TS_OK)
    return st;
  vm_reset_run (vm);
  vm->jit = true;
  st = exec_list (vm, prog->first, error);
  vm->jit = false;
  st = ts_rt_finish (vm, st, error);
  ts_node_free (prog);
  if (st != TS_OK)
    return st;
  if (output)
    {
      *output = strdup (vm->output ? vm->output : "");
      if (!*output)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  return TS_OK;
}

TS_Status
ts_vm_run_backend (TS_VM *vm, const char *source, TS_Backend backend,
                   char **output, TS_Error *error)
{
  switch (backend)
    {
    case TS_BACKEND_VM:
      return ts_vm_run_string (vm, source, output, error);
    case TS_BACKEND_JITERATI:
      return ts_vm_run_jiterati (vm, source, output, error);
    case TS_BACKEND_C:
    case TS_BACKEND_QBE:
      ts_error_set (
          error, TS_ERR_UNSUPPORTED, 0, 0,
          "backend '%s' is a text/AOT backend; use ts_vm_compile_backend",
          ts_backend_name (backend));
      return TS_ERR_UNSUPPORTED;
    default:
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "unknown Termscript backend: %d", (int)backend);
      return TS_ERR_UNSUPPORTED;
    }
}

TS_Status
ts_vm_add_search_path (TS_VM *vm, const char *dir, TS_Error *error)
{
  char *copy;
  char **nv;
  size_t i;
  if (!vm || !dir || !*dir)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad search path");
      return TS_ERR_INVAL;
    }
  for (i = 0; i < vm->nspaths; i++)
    if (strcmp (vm->spaths[i], dir) == 0)
      return TS_OK; /* Idempotent: duplicates are ignored. */
  copy = strdup (dir);
  if (!copy)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (vm->nspaths == vm->spaths_cap)
    {
      size_t nc = vm->spaths_cap ? vm->spaths_cap * 2 : 4;
      nv = realloc (vm->spaths, nc * sizeof *nv);
      if (!nv)
        {
          free (copy);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->spaths = nv;
      vm->spaths_cap = nc;
    }
  vm->spaths[vm->nspaths++] = copy;
  return TS_OK;
}

/* Names may only name a stdlib leaf: no directories, no escapes. */
static bool
ts_import_name_ok (const char *name)
{
  size_t i;
  if (!name || !*name || strlen (name) > 128)
    return false;
  if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
    return false;
  for (i = 0; name[i]; i++)
    {
      char c = name[i];
      if (!(isalnum ((unsigned char)c) || c == '_' || c == '-' || c == '.'))
        return false;
    }
  return true;
}

/* Read a whole file (caller frees). NULL with error set on failure. */
static char *
ts_slurp (const char *path, TS_Error *error)
{
  FILE *f = fopen (path, "rb");
  char *buf;
  size_t cap = 4096, len = 0, r;
  if (!f)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "cannot open '%s'", path);
      return NULL;
    }
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  while ((r = fread (buf + len, 1, cap - len - 1, f)) > 0)
    {
      char *nb;
      len += r;
      if (len + 1 < cap)
        continue;
      if (cap >= TS_MAX_FILE)
        {
          free (buf);
          fclose (f);
          ts_error_set (error, TS_ERR_LIMIT, 0, 0, "script too large");
          return NULL;
        }
      cap *= 2;
      nb = realloc (buf, cap);
      if (!nb)
        {
          free (buf);
          fclose (f);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return NULL;
        }
      buf = nb;
    }
  fclose (f);
  buf[len] = '\0';
  return buf;
}

/* Try "<dir>/<name>.tsc" then "<dir>/<name>/<name>.tsc".  Returns a
 * malloc'd path on success, NULL otherwise (no error details: callers
 * keep scanning further directories). */
static char *
ts_probe_dir (const char *dir, const char *name)
{
  static const char *const layouts[] = { "%s/%s.tsc", "%s/%s/%s.tsc" };
  size_t li;
  for (li = 0; li < 2; li++)
    {
      char path[4096];
      FILE *probe;
      int n;
      if (li == 0)
        n = snprintf (path, sizeof path, layouts[0], dir, name);
      else
        n = snprintf (path, sizeof path, layouts[1], dir, name, name);
      if (n < 0 || (size_t)n >= sizeof path)
        continue;
      probe = fopen (path, "rb");
      if (probe)
        {
          fclose (probe);
          return strdup (path);
        }
    }
  return NULL;
}

TS_Status
ts_vm_import (TS_VM *vm, const char *name, TS_Error *error)
{
  char *path = NULL;
  char *text = NULL;
  TS_Status st;
  size_t i;
  if (!vm || !ts_import_name_ok (name))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad import name '%s'",
                    name ? name : "(null)");
      return TS_ERR_INVAL;
    }
  for (i = 0; i < vm->nspaths && !path; i++)
    path = ts_probe_dir (vm->spaths[i], name);
  if (!path)
    {
      /* TERMSCRIPT_PATH is consulted on every import so tests and
       * install trees can relocate the stdlib without VM state. */
      const char *env = getenv ("TERMSCRIPT_PATH");
      char *dirs = env ? strdup (env) : NULL;
      if (dirs)
        {
          char *save = NULL, *d = strtok_r (dirs, ":", &save);
          while (d && !path)
            {
              if (*d)
                path = ts_probe_dir (d, name);
              d = strtok_r (NULL, ":", &save);
            }
          free (dirs);
        }
    }
  if (!path)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0,
                    "stdlib module '%s' not found", name);
      return TS_ERR_NOTFOUND;
    }
  for (i = 0; i < vm->nistack; i++)
    if (strcmp (vm->istack[i], path) == 0)
      {
        free (path);
        ts_error_set (error, TS_ERR_INVAL, 0, 0, "import cycle on '%s'", name);
        return TS_ERR_INVAL;
      }
  text = ts_slurp (path, error);
  if (!text)
    {
      TS_Status code = error ? error->code : TS_ERR_SYSTEM;
      free (path);
      return code;
    }
  if (vm->nistack == vm->istack_cap)
    {
      size_t nc = vm->istack_cap ? vm->istack_cap * 2 : 4;
      char **nv = realloc (vm->istack, nc * sizeof *nv);
      if (!nv)
        {
          free (text);
          free (path);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->istack = nv;
      vm->istack_cap = nc;
    }
  vm->istack[vm->nistack++] = path; /* Owned until pop below. */
  st = ts_exec_source (vm, text, false, error);
  free (text);
  free (vm->istack[--vm->nistack]);
  if (st != TS_OK)
    return st;
  return TS_OK;
}

TS_Status
ts_check_syntax (const char *source, TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Status st;
  if (!source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  st = ts_parse_program (source, &prog, error);
  ts_node_free (prog);
  return st;
}

/* ---------------- C backend (AOT transpiler) ----------------
 *
 * ts_vm_compile_to_c compiles Termscript to real C: statements become
 * C control flow and every expression becomes value construction plus
 * calls through the ts_rt_* compiled-code runtime above.  No source
 * text is embedded and nothing is parsed at run time.
 *
 * Non-native libraries (`.tsc` companions loaded with
 * `G:import "name"`) are compiled too: constant-string `G:import`
 * sites are resolved through the compile-time search paths (the VM
 * paths plus TERMSCRIPT_PATH) and their statements are inlined as
 * compiled C.  Anything else (dynamic names, cycles, missing files,
 * unparsable companions) stays a run-time `G:import` call with
 * identical VM semantics.  Native modules are never embedded: the
 * generated unit links them (statically) and resolves them through
 * the VM registry at run time.
 *
 * The same emitter serves the Termlib backend (dt_mode): only type
 * and helper names change (DT_TermValue / dt_rt_* / DT_*).  See
 * dt_termscript_compile_to_c, which forwards here.
 */

struct ts_cg_paths
{
  char **v;
  size_t n;
  size_t cap;
};

static void
ts_cg_paths_free (struct ts_cg_paths *p)
{
  size_t i;
  if (!p)
    return;
  for (i = 0; i < p->n; i++)
    free (p->v[i]);
  free (p->v);
  p->v = NULL;
  p->n = p->cap = 0;
}

static bool
ts_cg_paths_add (struct ts_cg_paths *p, const char *dir)
{
  size_t i;
  char *copy;
  char **nv;
  for (i = 0; i < p->n; i++)
    if (strcmp (p->v[i], dir) == 0)
      return true;
  copy = strdup (dir);
  if (!copy)
    return false;
  if (p->n == p->cap)
    {
      size_t nc = p->cap ? p->cap * 2 : 4;
      nv = realloc (p->v, nc * sizeof *nv);
      if (!nv)
        {
          free (copy);
          return false;
        }
      p->v = nv;
      p->cap = nc;
    }
  p->v[p->n++] = copy;
  return true;
}

/* Resolve `name` like ts_vm_import: each search dir in order, then
 * TERMSCRIPT_PATH.  Returns a malloc'd path or NULL. */
static char *
ts_cg_resolve (struct ts_cg_paths *p, const char *name)
{
  char *path = NULL;
  size_t i;
  for (i = 0; i < p->n && !path; i++)
    path = ts_probe_dir (p->v[i], name);
  if (!path)
    {
      const char *env = getenv ("TERMSCRIPT_PATH");
      char *dirs = env ? strdup (env) : NULL;
      if (dirs)
        {
          char *save = NULL, *d = strtok_r (dirs, ":", &save);
          while (d && !path)
            {
              if (*d)
                path = ts_probe_dir (d, name);
              d = strtok_r (NULL, ":", &save);
            }
          free (dirs);
        }
    }
  return path;
}

static bool
ts_cg_stack_has (char **stack, size_t nstack, const char *path)
{
  size_t i;
  for (i = 0; i < nstack; i++)
    if (strcmp (stack[i], path) == 0)
      return true;
  return false;
}

static bool
ts_cg_is_bare_g_call (ts_node_t *s, const char *func, const char **str_out)
{
  ts_node_t *rhs;
  if (!s || s->kind != N_EXPR_STMT)
    return false;
  rhs = s->rhs;
  if (!rhs || rhs->kind != N_CALL || !rhs->module || !rhs->func)
    return false;
  if (strcmp (rhs->module, "G") != 0 || strcmp (rhs->func, func) != 0)
    return false;
  if (rhs->nargs != 1 || rhs->args[0].tag != 2 || !rhs->args[0].string)
    return false;
  if (str_out)
    *str_out = rhs->args[0].string;
  return true;
}

#define TS_CG_MAX_INLINE_DEPTH 16

/* Inline constant-string `G:import` companions into the chain in
 * place; `*head` is the chain head pointer.  `paths` evolves as
 * `G:import_path` statements are seen (mirroring run-time order). */
static void
ts_cg_inline_chain (ts_node_t **head, struct ts_cg_paths *paths, char **stack,
                    size_t nstack, unsigned depth)
{
  ts_node_t **pp = head;
  while (*pp)
    {
      ts_node_t *s = *pp;
      const char *str = NULL;
      if (s->kind == N_EXPR_STMT && s->rhs && s->rhs->kind == N_BLOCK)
        {
          ts_cg_inline_chain (&s->rhs->first, paths, stack, nstack, depth);
          pp = &s->next;
          continue;
        }
      if (s->kind == N_WHILE)
        {
          ts_cg_inline_chain (&s->first, paths, stack, nstack, depth);
          pp = &s->next;
          continue;
        }
      if (s->kind == N_IF)
        {
          ts_cg_inline_chain (&s->first, paths, stack, nstack, depth);
          ts_cg_inline_chain (&s->otherwise, paths, stack, nstack, depth);
          pp = &s->next;
          continue;
        }
      if (ts_cg_is_bare_g_call (s, "import_path", &str))
        {
          /* Compile-time search path tracks run time; the statement
           * itself still runs (later dynamic imports need it). */
          ts_cg_paths_add (paths, str);
          pp = &s->next;
          continue;
        }
      if (!ts_cg_is_bare_g_call (s, "import", &str) || !ts_import_name_ok (str)
          || depth > TS_CG_MAX_INLINE_DEPTH)
        {
          pp = &s->next;
          continue;
        }
      {
        char *found = ts_cg_resolve (paths, str);
        char *text = NULL;
        ts_node_t *prog = NULL, *chain = NULL;
        TS_Error dummy;
        if (!found || ts_cg_stack_has (stack, nstack, found))
          {
            free (found);
            pp = &s->next;
            continue;
          }
        ts_error_clear (&dummy);
        text = ts_slurp (found, &dummy);
        if (!text)
          {
            free (found);
            pp = &s->next; /* Too large / unreadable: run time reports. */
            continue;
          }
        TS_Status parse_status = ts_parse_program (text, &prog, &dummy);
        free (text);
        if (parse_status != TS_OK)
          {
            /* Unparsable companion: leave the run-time import, which
             * fails identically when executed. */
            free (found);
            pp = &s->next;
            continue;
          }
        stack[nstack] = found;
        ts_cg_inline_chain (&prog->first, paths, stack, nstack + 1, depth + 1);
        chain = prog->first;
        prog->first = NULL;
        ts_node_free (prog);
        /* Keep the import statement/expression charge, including empty files.
         * The compiled block executes in the caller's managed lifetime. */
        s->rhs->kind = N_BLOCK;
        s->rhs->first = chain;
        pp = &s->next;
        free (found);
      }
    }
}

/* Growable C-text buffer with a sticky OOM flag. */
typedef struct
{
  char *buf;
  size_t len;
  size_t cap;
  unsigned tmp;
  unsigned failure; /* 0: unit failure; otherwise scoped cleanup label. */
  bool dt_mode;
  bool function;
  bool failed;
} ts_cg_t;

static void
ts_cg_raw_put (ts_cg_t *g, const char *s, size_t n)
{
  if (g->failed)
    return;
  if (g->len + n + 1 > g->cap)
    {
      size_t nc = g->cap ? g->cap * 2 : 1024;
      char *nb;
      while (nc < g->len + n + 1)
        nc *= 2;
      nb = realloc (g->buf, nc);
      if (!nb)
        {
          g->failed = true;
          return;
        }
      g->buf = nb;
      g->cap = nc;
    }
  memcpy (g->buf + g->len, s, n);
  g->len += n;
  g->buf[g->len] = '\0';
}

/* Redirect code branches to the nearest owned-value cleanup. Literal
 * payloads are emitted bytewise by ts_cg_cstr and are never rewritten. */
static void
ts_cg_put (ts_cg_t *g, const char *s, size_t n)
{
  const char needle[] = "goto fail;";
  if (!g->failure)
    {
      ts_cg_raw_put (g, s, n);
      return;
    }
  size_t start = 0;
  for (size_t i = 0; i + sizeof needle - 1 <= n; i++)
    if (!memcmp (s + i, needle, sizeof needle - 1))
      {
        char jump[64];
        int len = snprintf (jump, sizeof jump, "goto ts_fail%u;", g->failure);
        ts_cg_raw_put (g, s + start, i - start);
        ts_cg_raw_put (g, jump, (size_t)len);
        i += sizeof needle - 2;
        start = i + 1;
      }
  ts_cg_raw_put (g, s + start, n - start);
}

static void
ts_cg_str (ts_cg_t *g, const char *s)
{
  ts_cg_put (g, s, strlen (s));
}

static void
ts_cg_fmt (ts_cg_t *g, const char *fmt, ...)
{
  char tmp[1024];
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = vsnprintf (tmp, sizeof tmp, fmt, ap);
  va_end (ap);
  if (n < 0)
    {
      g->failed = true;
      return;
    }
  if ((size_t)n >= sizeof tmp)
    {
      /* Long names/numbers only: heap path. */
      char *big = malloc ((size_t)n + 1);
      if (!big)
        {
          g->failed = true;
          return;
        }
      va_start (ap, fmt);
      vsnprintf (big, (size_t)n + 1, fmt, ap);
      va_end (ap);
      ts_cg_put (g, big, (size_t)n);
      free (big);
      return;
    }
  ts_cg_put (g, tmp, (size_t)n);
}

/* Name selectors for the two flavors. */
static const char *
ts_cg_V (ts_cg_t *g)
{
  return g->dt_mode ? "DT_TermValue" : "TS_Value";
}
static const char *
ts_cg_S (ts_cg_t *g)
{
  return g->dt_mode ? "DT_Status" : "TS_Status";
}

static void
ts_cg_cstr (ts_cg_t *g, const char *s)
{
  /* C string literal with octal escapes (always 3 digits, so no
   * \x-style run-on into following hex digits). */
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  ts_cg_str (g, "\"");
  for (; *p; p++)
    {
      switch (*p)
        {
        case '"':
          ts_cg_str (g, "\\\"");
          break;
        case '\\':
          ts_cg_str (g, "\\\\");
          break;
        case '\n':
          ts_cg_str (g, "\\n");
          break;
        case '\t':
          ts_cg_str (g, "\\t");
          break;
        case '\r':
          ts_cg_str (g, "\\r");
          break;
        default:
          if (*p < 0x20 || *p > 0x7E)
            ts_cg_fmt (g, "\\%03o", *p);
          else
            ts_cg_put (g, (const char *)p, 1);
          break;
        }
    }
  ts_cg_str (g, "\"");
}

static unsigned
ts_cg_temp (ts_cg_t *g)
{
  return g->tmp++;
}

static void
ts_cg_integer (ts_cg_t *g, const char *out, int64_t value)
{
  ts_cg_fmt (g, "%s.type = %s; %s.as.integer = ", out,
             g->dt_mode ? "DT_TERM_INT" : "TS_INT", out);
  if (value == INT64_MIN)
    ts_cg_str (g, "(-9223372036854775807LL - 1LL)");
  else
    ts_cg_fmt (g, "%lldLL", (long long)value);
  ts_cg_str (g, ";\n");
}

/* Short aliases so the emitter reads uniformly. */
#define CG_VMN(g) ((g)->dt_mode ? "dt_vm" : "ts_vm")
#define CG_ERN(g) ((g)->dt_mode ? "dt_err" : "ts_err")
#define CG_OK(g) ((g)->dt_mode ? "DT_OK" : "TS_OK")
#define CG_NOMEM(g) ((g)->dt_mode ? "DT_ERR_NO_MEMORY" : "TS_ERR_NOMEM")
#define CG_CALL(g) ((g)->dt_mode ? "dt_rt_call" : "ts_rt_call")
#define CG_GET(g) ((g)->dt_mode ? "dt_rt_get" : "ts_rt_get")
#define CG_SET(g) ((g)->dt_mode ? "dt_rt_set" : "ts_rt_set")
#define CG_STEP(g) ((g)->dt_mode ? "dt_rt_step" : "ts_rt_step")
#define CG_FREE(g) ((g)->dt_mode ? "dt_termvalue_free" : "ts_value_free")
#define CG_MKSTR(g) ((g)->dt_mode ? "dt_rt_mkstring" : "ts_value_make_string")
#define CG_TRUTHY(g) ((g)->dt_mode ? "dt_rt_truthy" : "ts_value_truthy")
#define CG_ERRSET(g) ((g)->dt_mode ? "dt_error_set" : "ts_error_set")
#define CG_PARSE(g) ((g)->dt_mode ? "DT_ERR_PARSE" : "TS_ERR_PARSE")
#define CG_LIFETIME(g)                                                        \
  ((g)->dt_mode ? "dt_rt_lifetime_set" : "ts_rt_lifetime_set")
#define CG_FIELD(g) ((g)->dt_mode ? "dt_rt_field" : "ts_rt_field")
#define CG_ENTER(g) ((g)->dt_mode ? "dt_rt_flow_enter" : "ts_rt_flow_enter")
#define CG_LEAVE(g) ((g)->dt_mode ? "dt_rt_flow_leave" : "ts_rt_flow_leave")
#define CG_FINISH(g) ((g)->dt_mode ? "dt_rt_finish" : "ts_rt_finish")

static void ts_cg_list (ts_cg_t *g, ts_node_t *list);
static void ts_cg_expr (ts_cg_t *g, ts_node_t *n, const char *out);

/* Emit one call argument into OUT (a declared value slot).  On failure
 * the snippet jumps to `fail` after running CLEANUP. */
static void
ts_cg_arg (ts_cg_t *g, ts_arg_t *a, const char *out, unsigned line,
           const char *cleanup)
{
  if (a->tag == 5)
    {
      unsigned label = ts_cg_temp (g) + 1, parent = g->failure;
      g->failure = label;
      ts_cg_expr (g, a->expr, out);
      g->failure = parent;
      ts_cg_fmt (
          g,
          "goto ts_arg_done%u;\nts_fail%u:\n%s (&%s); %s\nts_arg_done%u:;\n",
          label, label, CG_FREE (g), out, cleanup, label);
    }
  else if (a->tag == 0)
    {
      ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) { %s }\n",
                 CG_GET (g), CG_VMN (g), a->name, out, line, CG_ERN (g),
                 CG_OK (g), cleanup);
    }
  else if (a->tag == 1)
    {
      ts_cg_integer (g, out, a->number);
    }
  else if (a->tag == 2)
    {
      /* dt_rt_mkstring takes the error param and reports itself;
       * ts_value_make_string does not, so the caller reports NOMEM. */
      ts_cg_fmt (g, "if (%s (&%s, ", CG_MKSTR (g), out);
      ts_cg_cstr (g, a->string);
      if (g->dt_mode)
        ts_cg_fmt (g, ", %s) != %s) { %s }\n", CG_ERN (g), CG_OK (g), cleanup);
      else
        ts_cg_fmt (
            g, ") != %s) { %s (%s, %s, ENOMEM, 0, \"out of memory\"); %s }\n",
            CG_OK (g), CG_ERRSET (g), CG_ERN (g), CG_NOMEM (g), cleanup);
    }
  else if (a->tag == 3)
    {
      ts_cg_fmt (g, "%s.type = %s; %s.as.boolean = %s;\n", out,
                 g->dt_mode ? "DT_TERM_BOOL" : "TS_BOOL", out,
                 a->boolean ? "true" : "false");
    }
  else
    ts_cg_fmt (g, "%s.type = %s;\n", out,
               g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
}

static void
ts_cg_call (ts_cg_t *g, ts_node_t *n, const char *out)
{
  unsigned m = ts_cg_temp (g);
  size_t i;
  char mv[64], av[64], rv[64], sv[64];
  snprintf (mv, sizeof mv, "ts_m%u", m);
  snprintf (av, sizeof av, "ts_a%u", m);
  snprintf (rv, sizeof rv, "ts_r%u", m);
  snprintf (sv, sizeof sv, "ts_s%u", m);
  ts_cg_fmt (g, "{\n%s %s; %s %s;\n", ts_cg_V (g), mv, ts_cg_V (g), rv);
  if (n->nargs)
    ts_cg_fmt (g,
               "%s %s[%zu]; size_t ts_k%u;\nfor (ts_k%u = 0; ts_k%u < %zu; "
               "ts_k%u++) %s[ts_k%u].type = %s;\n",
               ts_cg_V (g), av, n->nargs, m, m, m, n->nargs, m, av, m,
               g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
  ts_cg_fmt (g, "%s %s;\n", ts_cg_S (g), sv);
  ts_cg_fmt (g, "%s.type = %s; %s.type = %s;\n", mv,
             g->dt_mode ? "DT_TERM_NIL" : "TS_NIL", rv,
             g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
  ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) goto fail;\n",
             CG_GET (g), CG_VMN (g), n->module, mv, n->line, CG_ERN (g),
             CG_OK (g));
  for (i = 0; i < n->nargs; i++)
    {
      char slot[96], cl[512];
      snprintf (slot, sizeof slot, "%s[%zu]", av, i);
      if (i == 0)
        snprintf (cl, sizeof cl, "%s (&%s); goto fail;", CG_FREE (g), mv);
      else
        snprintf (cl, sizeof cl,
                  "%s (&%s); { size_t ts_j%u; for (ts_j%u = 0; ts_j%u < %zu; "
                  "ts_j%u++) %s (&%s[ts_j%u]); } goto fail;",
                  CG_FREE (g), mv, m, m, m, i, m, CG_FREE (g), av, m);
      ts_cg_arg (g, &n->args[i], slot, n->line, cl);
    }
  ts_cg_fmt (g, "%s = %s (%s, &%s, \"%s\", \"%s\", %s, %zu, &%s, %u, %s);\n",
             sv, CG_CALL (g), CG_VMN (g), mv, n->module, n->func,
             n->nargs ? av : "NULL", n->nargs, rv, n->line, CG_ERN (g));
  if (n->nargs)
    ts_cg_fmt (g,
               "%s (&%s); { size_t ts_k%u; for (ts_k%u = 0; ts_k%u < %zu; "
               "ts_k%u++) %s (&%s[ts_k%u]); }\n",
               CG_FREE (g), mv, m, m, m, n->nargs, m, CG_FREE (g), av, m);
  else
    ts_cg_fmt (g, "%s (&%s);\n", CG_FREE (g), mv);
  ts_cg_fmt (
      g, "if (%s != %s) { %s (&%s); goto fail; }\n%s = %s; %s.type = %s;\n}\n",
      sv, CG_OK (g), CG_FREE (g), rv, out, rv, rv,
      g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
}

static void
ts_cg_expr (ts_cg_t *g, ts_node_t *n, const char *out)
{
  if (!n)
    return; /* Destination starts as nil. */
  ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g), CG_VMN (g),
             CG_ERN (g), CG_OK (g));
  switch (n->kind)
    {
    case N_VAR:
      ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) goto fail;\n",
                 CG_GET (g), CG_VMN (g), n->name, out, n->line, CG_ERN (g),
                 CG_OK (g));
      break;
    case N_LIT_INT:
      ts_cg_integer (g, out, n->number);
      break;
    case N_LIT_STR:
      ts_cg_fmt (g, "if (%s (&%s, ", CG_MKSTR (g), out);
      ts_cg_cstr (g, n->string);
      if (g->dt_mode)
        ts_cg_fmt (g, ", %s) != %s) goto fail;\n", CG_ERN (g), CG_OK (g));
      else
        ts_cg_fmt (g,
                   ") != %s) { %s (%s, %s, ENOMEM, 0, \"out of memory\"); "
                   "goto fail; }\n",
                   CG_OK (g), CG_ERRSET (g), CG_ERN (g), CG_NOMEM (g));
      break;
    case N_LIT_BOOL:
      ts_cg_fmt (g, "%s.type = %s; %s.as.boolean = %s;\n", out,
                 g->dt_mode ? "DT_TERM_BOOL" : "TS_BOOL", out,
                 n->boolean ? "true" : "false");
      break;
    case N_LIT_NIL:
      ts_cg_fmt (g, "%s.type = %s;\n", out,
                 g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
      break;
    case N_CALL:
      ts_cg_call (g, n, out);
      break;
    case N_BLOCK:
      ts_cg_list (g, n->first);
      break;
    case N_FIELD:
      ts_cg_fmt (g, "if (%s (%s, ", CG_FIELD (g), CG_VMN (g));
      ts_cg_cstr (g, n->name);
      ts_cg_str (g, ", ");
      ts_cg_cstr (g, n->func);
      ts_cg_fmt (g, ", &%s, %u, %s) != %s) goto fail;\n", out, n->line,
                 CG_ERN (g), CG_OK (g));
      break;
    case N_NOT:
      {
        unsigned m = ts_cg_temp (g);
        char v[64];
        snprintf (v, sizeof v, "ts_not%u", m);
        ts_cg_fmt (g, "{ %s %s = {0};\n", ts_cg_V (g), v);
        ts_cg_expr (g, n->lhs, v);
        ts_cg_fmt (g, "%s.type = %s; %s.as.boolean = !%s (&%s); %s (&%s); }\n",
                   out, g->dt_mode ? "DT_TERM_BOOL" : "TS_BOOL", out,
                   CG_TRUTHY (g), v, CG_FREE (g), v);
      }
      break;
    case N_FLOW:
      {
        unsigned m = ts_cg_temp (g) + 1, parent = g->failure;
        unsigned callback = ts_cg_temp (g) + 1;
        char v[64], ignored[64];
        snprintf (v, sizeof v, "ts_flow%u", m);
        snprintf (ignored, sizeof ignored, "ts_ignore%u", m);
        ts_cg_fmt (g,
                   "{ %s %s = {0}, %s = {0}; %s ts_original%u = %s, "
                   "ts_status%u = %s;\n",
                   ts_cg_V (g), v, ignored, ts_cg_S (g), m, CG_OK (g), m,
                   CG_OK (g));
        g->failure = m;
        ts_cg_expr (g, n->lhs, v);
        g->failure = parent;
        ts_cg_fmt (g,
                   "goto ts_flow_join%u;\nts_fail%u:\nts_original%u = "
                   "%s->code;\nts_flow_join%u:;\n",
                   m, m, m, CG_ERN (g), m);
        ts_cg_fmt (g,
                   "if (%s (%s, &%s, ts_original%u, %s) != %s) { %s (&%s); "
                   "goto fail; }\n",
                   CG_ENTER (g), CG_VMN (g), v, m, CG_ERN (g), CG_OK (g),
                   CG_FREE (g), v);
        ts_cg_fmt (g, "if (ts_original%u == %s) {\n", m, CG_OK (g));
        g->failure = callback;
        ts_cg_expr (g, n->rhs, ignored);
        ts_cg_str (g, "} else {\n");
        ts_cg_expr (g, n->first, ignored);
        ts_cg_fmt (g, "%s (&%s);\n", CG_FREE (g), ignored);
        ts_cg_expr (g, n->otherwise, ignored);
        g->failure = parent;
        ts_cg_fmt (g,
                   "}\ngoto ts_callback_join%u;\nts_fail%u:\nts_status%u = "
                   "%s->code;\nts_callback_join%u:;\n",
                   m, callback, m, CG_ERN (g), m);
        ts_cg_fmt (g, "%s (&%s); ts_status%u = %s (%s, ts_status%u, %s);\n",
                   CG_FREE (g), ignored, m, CG_LEAVE (g), CG_VMN (g), m,
                   CG_ERN (g));
        ts_cg_fmt (g,
                   "if (ts_status%u != %s) { %s (&%s); goto fail; }\n%s = %s; "
                   "%s.type = 0; }\n",
                   m, CG_OK (g), CG_FREE (g), v, out, v, v);
      }
      break;
    case N_AND:
    case N_OR:
      {
        unsigned m = ts_cg_temp (g);
        char lv[64];
        snprintf (lv, sizeof lv, "ts_l%u", m);
        ts_cg_fmt (g, "{\n%s %s; %s.type = %s;\n", ts_cg_V (g), lv, lv,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_expr (g, n->lhs, lv);
        ts_cg_fmt (
            g, "if (%s%s (&%s)) { %s = %s; %s.type = %s; }\nelse { %s (&%s); ",
            n->kind == N_AND ? "!" : "", CG_TRUTHY (g), lv, out, lv, lv,
            g->dt_mode ? "DT_TERM_NIL" : "TS_NIL", CG_FREE (g), lv);
        ts_cg_expr (g, n->rhs, out);
        ts_cg_str (g, "}\n}\n");
      }
      break;
    default:
      ts_cg_fmt (g, "%s (%s, %s, 0, %u, \"not an expression\"); goto fail;\n",
                 CG_ERRSET (g), CG_ERN (g), CG_PARSE (g), n->line);
      break;
    }
}

static void
ts_cg_stmt (ts_cg_t *g, ts_node_t *s)
{
  switch (s->kind)
    {
    case N_FUNCTION:
      ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                 CG_VMN (g), CG_ERN (g), CG_OK (g));
      ts_cg_fmt (
          g, "if (%s (%s, \"%s\", ts_function_%lld, %s) != %s) goto fail;\n",
          g->dt_mode ? "dt_rt_function" : "ts_rt_function", CG_VMN (g),
          s->name, (long long)s->number, CG_ERN (g), CG_OK (g));
      break;
    case N_RETURN:
      if (!g->function)
        {
          g->failed = true;
          break;
        }
      ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                 CG_VMN (g), CG_ERN (g), CG_OK (g));
      ts_cg_expr (g, s->rhs, "(*ts_ret)");
      ts_cg_str (g, "return TS_OK;\n");
      break;
    case N_CONST:
    case N_ASSIGN:
    case N_MANAGED:
      {
        unsigned m = ts_cg_temp (g);
        bool is_const = s->kind == N_CONST;
        ts_cg_fmt (g, "{\n%s ts_e%u; ts_e%u.type = %s;\n", ts_cg_V (g), m, m,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_e%u", m);
          ts_cg_expr (g, s->rhs, slot);
        }
        if (s->kind == N_MANAGED)
          ts_cg_fmt (g,
                     "if (%s (%s, \"%s\", &ts_e%u, %u, %s) != %s) { %s "
                     "(&ts_e%u); goto fail; }\n",
                     CG_LIFETIME (g), CG_VMN (g), s->name, m, s->line,
                     CG_ERN (g), CG_OK (g), CG_FREE (g), m);
        else
          ts_cg_fmt (g,
                     "if (%s (%s, \"%s\", &ts_e%u, %s, %u, %s) != %s) { %s "
                     "(&ts_e%u); goto fail; }\n",
                     CG_SET (g), CG_VMN (g), s->name, m,
                     is_const ? "true" : "false", s->line, CG_ERN (g),
                     CG_OK (g), CG_FREE (g), m);
        ts_cg_str (g, "}\n");
      }
      break;
    case N_EXPR_STMT:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "{\n%s ts_e%u; ts_e%u.type = %s;\n", ts_cg_V (g), m, m,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_e%u", m);
          ts_cg_expr (g, s->rhs, slot);
        }
        ts_cg_fmt (g, "%s (&ts_e%u);\n}\n", CG_FREE (g), m);
      }
      break;
    case N_WHILE:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        ts_cg_fmt (g, "for (;;) {\n%s ts_c%u; ts_c%u.type = %s;\n",
                   ts_cg_V (g), m, m, g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_c%u", m);
          ts_cg_expr (g, s->cond, slot);
        }
        ts_cg_fmt (g,
                   "{ bool ts_g%u = %s (&ts_c%u) ? true : false; %s "
                   "(&ts_c%u); if (!ts_g%u) break; }\n",
                   m, CG_TRUTHY (g), m, CG_FREE (g), m, m);
        ts_cg_list (g, s->first);
        ts_cg_str (g, "}\n");
      }
      break;
    case N_IF:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "{\n%s ts_c%u; ts_c%u.type = %s;\nbool ts_g%u;\n",
                   ts_cg_V (g), m, m, g->dt_mode ? "DT_TERM_NIL" : "TS_NIL",
                   m);
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_c%u", m);
          ts_cg_expr (g, s->cond, slot);
        }
        ts_cg_fmt (g,
                   "ts_g%u = %s (&ts_c%u) ? true : false; %s (&ts_c%u);\nif "
                   "(ts_g%u) {\n",
                   m, CG_TRUTHY (g), m, CG_FREE (g), m, m);
        ts_cg_list (g, s->first);
        if (s->otherwise)
          {
            ts_cg_str (g, "} else {\n");
            ts_cg_list (g, s->otherwise);
            ts_cg_str (g, "}\n");
          }
        else
          ts_cg_str (g, "}\n");
        ts_cg_str (g, "}\n");
      }
      break;
    default:
      ts_cg_fmt (g, "%s (%s, %s, 0, %u, \"bad statement\"); goto fail;\n",
                 CG_ERRSET (g), CG_ERN (g), CG_PARSE (g), s->line);
      break;
    }
}

static void
ts_cg_list (ts_cg_t *g, ts_node_t *list)
{
  ts_node_t *s;
  for (s = list; s && !g->failed; s = s->next)
    ts_cg_stmt (g, s);
}

static void
ts_cg_functions (ts_cg_t *g, ts_node_t *list)
{
  for (ts_node_t *n = list; n; n = n->next)
    {
      if (n->kind != N_FUNCTION)
        {
          if (n->kind == N_EXPR_STMT && n->rhs && n->rhs->kind == N_BLOCK)
            ts_cg_functions (g, n->rhs->first);
          continue;
        }
      n->number = ts_cg_temp (g);
      bool dt = g->dt_mode;
      g->dt_mode = false;
      g->function = true;
      ts_cg_fmt (g,
                 "static TS_Status ts_function_%lld (TS_VM *ts_vm, void "
                 "*ts_ud, const TS_Value *ts_args, size_t ts_argc, TS_Value "
                 "*ts_ret, TS_Error *ts_err) {\n(void)ts_ud; "
                 "(void)ts_ret;\nif (ts_check_argc(ts_vm, ts_args, ts_argc, "
                 "%zu, ts_err) != TS_OK) goto fail;\n",
                 (long long)n->number, n->nargs);
      for (size_t i = 0; i < n->nargs; i++)
        ts_cg_fmt (g,
                   "if (ts_rt_param(ts_vm, \"%s\", &ts_args[%zu], ts_err) != "
                   "TS_OK) goto fail;\n",
                   n->args[i].name, i);
      ts_cg_list (g, n->first);
      ts_cg_str (g, "return TS_OK;\nfail: return ts_err->code == TS_OK ? "
                    "TS_ERR_NOMEM : ts_err->code;\n}\n");
      g->dt_mode = dt;
      g->function = false;
    }
}

/* Emit the whole unit.  `vm` supplies compile-time search paths. */
static const char *
ts_cg_emit (TS_VM *vm, ts_node_t *prog, const char *unit_name, bool dt_mode,
            TS_Error *error)
{
  ts_cg_t g;
  char ident[128];
  size_t i;
  struct ts_cg_paths paths;
  char *stack[64];
  memset (&g, 0, sizeof g);
  g.dt_mode = dt_mode;
  memset (&paths, 0, sizeof paths);
  for (i = 0; i < vm->nspaths; i++)
    ts_cg_paths_add (&paths, vm->spaths[i]);
  memset (stack, 0, sizeof stack);
  ts_cg_inline_chain (&prog->first, &paths, stack, 0, 0);
  ts_cg_paths_free (&paths);
  if (!unit_name || !*unit_name)
    unit_name = "termscript_unit";
  for (i = 0; i + 1 < sizeof ident && unit_name[i]; i++)
    ident[i] = isalnum ((unsigned char)unit_name[i]) ? unit_name[i] : '_';
  ident[i] = '\0';
  if (dt_mode)
    ts_cg_str (
        &g, "/* Generated by the Termlib Termscript C backend (AOT).\n"
            " * Termscript statements were compiled to C; Termscript-side\n"
            " * libraries (`.tsc` companions) were inlined at compile time.\n"
            " * Native modules (std.*, custom) link statically and resolve\n"
            " * through the VM registry at run time.\n"
            " * Compile: cc -o UNIT UNIT.c -ltermscript "
            "-ltermscript_standalone -lnovoparse -ltermlib\n"
            " * Static:  cc -static -o UNIT UNIT.c -ltermscript "
            "-ltermscript_standalone -lnovoparse -ltermlib\n"
            " */\n"
            "#include <stdio.h>\n"
            "#include <stdlib.h>\n"
            "#include <string.h>\n"
            "#include <stdbool.h>\n"
            "#include <stddef.h>\n"
            "#include <stdint.h>\n"
            "#include <errno.h>\n"
            "#include <termlib.h>\n"
            "#include <termscript/termscript.h>\n"
            "extern DT_Status dt_rt_function(DT_TermVM *, const char *, "
            "TS_Func, DT_Error *);\n\n");
  else
    ts_cg_str (
        &g,
        "/* Generated by the Termscript C backend (AOT).\n"
        " * Termscript statements were compiled to C; Termscript-side\n"
        " * libraries (`.tsc` companions) were inlined at compile time.\n"
        " * Native modules link statically and resolve through the VM\n"
        " * registry at run time: weak references call\n"
        " * ts_stdlib_register_all/search_path when linked in, so\n"
        " * G-only units link libtermscript_standalone + libnovoparse\n"
        " * while std.* users add the stdlib (force archive extraction with\n"
        " * -u, since weak references alone do not pull it in):\n"
        " * Compile: cc -o UNIT UNIT.c -ltermscript_standalone -lnovoparse "
        "-lpthread\n"
        " *   (+ -u ts_stdlib_register_all -u ts_stdlib_search_path\n"
        " *      -ltermscript_stdlib for std.* native modules)\n"
        " * Static:  cc -static -o UNIT UNIT.c ... (same libraries)\n"
        " */\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <errno.h>\n"
        "#include <termscript/termscript.h>\n\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "__attribute__((weak)) TS_Status ts_stdlib_register_all (TS_VM *, "
        "TS_Error *);\n"
        "__attribute__((weak)) TS_Status ts_stdlib_search_path (TS_VM *, "
        "TS_Error *);\n"
        "#endif\n\n");
  ts_cg_functions (&g, prog->first);
  if (dt_mode)
    ts_cg_fmt (&g,
               "static DT_Status\ntsc_run_%s (DT_TermVM *dt_vm, DT_Error "
               "*dt_err)\n{\n(void) dt_vm; (void) dt_err;\n",
               ident);
  else
    ts_cg_fmt (&g,
               "static TS_Status\ntsc_run_%s (TS_VM *ts_vm, TS_Error "
               "*ts_err)\n{\n(void) ts_vm; (void) ts_err;\n",
               ident);
  ts_cg_list (&g, prog->first);
  /* Keeps `fail` referenced even for empty programs (avoids an
   * unused-label warning under -Wall). */
  ts_cg_str (&g, "if (0) goto fail;\n");
  ts_cg_fmt (&g,
             "return %s (%s, %s, %s);\nfail:\nreturn %s (%s, %s->code != %s ? "
             "%s->code : %s, %s);\n}\n\n",
             CG_FINISH (&g), CG_VMN (&g), CG_OK (&g), CG_ERN (&g),
             CG_FINISH (&g), CG_VMN (&g), CG_ERN (&g), CG_OK (&g), CG_ERN (&g),
             CG_NOMEM (&g), CG_ERN (&g));
  ts_cg_str (&g, "int\nmain (int argc, char **argv)\n{\n");
  if (dt_mode)
    {
      ts_cg_fmt (
          &g,
          "DT_TermVM *vm_%s;\nDT_Error err_%s;\n(void) argc; (void) argv;\n",
          ident, ident);
      ts_cg_fmt (&g,
                 "dt_error_clear (&err_%s);\nvm_%s = dt_termscript_create "
                 "(&err_%s);\n",
                 ident, ident, ident);
      ts_cg_fmt (&g,
                 "if (!vm_%s) { fprintf (stderr, \"termscript: %%s\\n\", "
                 "err_%s.message); return 1; }\n",
                 ident, ident);
      ts_cg_fmt (
          &g,
          "{ TS_Error arg_error = {0}; if "
          "(ts_vm_set_args(dt_termscript_inner_vm(vm_%s), argc > 0 ? "
          "(size_t)(argc - 1) : 0, (const char *const *)(argv + (argc > 0)), "
          "&arg_error) != TS_OK) { fprintf(stderr, \"termscript: %%s\\n\", "
          "arg_error.message); dt_termscript_free(vm_%s); return 1; } }\n",
          ident, ident);
      ts_cg_fmt (&g,
                 "if (tsc_run_%s (vm_%s, &err_%s) != DT_OK) {\n"
                 "fprintf (stderr, \"termscript: %%s\\n\", err_%s.message);\n"
                 "dt_termscript_free (vm_%s);\nreturn 1;\n}\n",
                 ident, ident, ident, ident, ident);
      ts_cg_fmt (&g,
                 "fputs (dt_rt_output (vm_%s) ? dt_rt_output (vm_%s) : \"\", "
                 "stdout);\n",
                 ident, ident);
      ts_cg_fmt (&g,
                 "int ts_exit_code = "
                 "ts_vm_exit_code(dt_termscript_inner_vm(vm_%s));\ndt_"
                 "termscript_free (vm_%s);\nreturn ts_exit_code;\n}\n",
                 ident, ident);
    }
  else
    {
      ts_cg_fmt (
          &g, "TS_VM *vm_%s;\nTS_Error err_%s;\n(void) argc; (void) argv;\n",
          ident, ident);
      ts_cg_fmt (
          &g, "ts_error_clear (&err_%s);\nvm_%s = ts_vm_create (&err_%s);\n",
          ident, ident, ident);
      ts_cg_fmt (&g,
                 "if (!vm_%s) { fprintf (stderr, \"termscript: %%s\\n\", "
                 "err_%s.message); return 1; }\n",
                 ident, ident);
      ts_cg_fmt (&g,
                 "if (ts_vm_set_args(vm_%s, argc > 0 ? (size_t)(argc - 1) : "
                 "0, (const char *const *)(argv + (argc > 0)), &err_%s) != "
                 "TS_OK) { ts_vm_free(vm_%s); return 1; }\n",
                 ident, ident, ident);
      ts_cg_str (&g, "#if defined(__GNUC__) || defined(__clang__)\n");
      ts_cg_fmt (&g,
                 "if (ts_stdlib_register_all) { if (ts_stdlib_register_all "
                 "(vm_%s, &err_%s) != TS_OK) "
                 "{ fprintf (stderr, \"termscript: %%s\\n\", err_%s.message); "
                 "ts_vm_free (vm_%s); return 1; } }\n",
                 ident, ident, ident, ident);
      ts_cg_fmt (&g,
                 "if (ts_stdlib_search_path) ts_stdlib_search_path (vm_%s, "
                 "&err_%s);\n",
                 ident, ident);
      ts_cg_str (&g, "#endif\n");
      ts_cg_fmt (&g,
                 "if (tsc_run_%s (vm_%s, &err_%s) != TS_OK) {\n"
                 "fprintf (stderr, \"termscript: %%s\\n\", err_%s.message);\n"
                 "ts_vm_free (vm_%s);\nreturn 1;\n}\n",
                 ident, ident, ident, ident, ident);
      ts_cg_fmt (&g,
                 "fputs (ts_rt_output (vm_%s) ? ts_rt_output (vm_%s) : \"\", "
                 "stdout);\n",
                 ident, ident);
      ts_cg_fmt (&g,
                 "int ts_exit_code = ts_vm_exit_code(vm_%s);\nts_vm_free "
                 "(vm_%s);\nreturn ts_exit_code;\n}\n",
                 ident, ident);
    }
  if (g.failed || !g.buf)
    {
      free (g.buf);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  free (vm->compiled);
  vm->compiled = g.buf;
  return vm->compiled;
}

const char *
ts_vm_compile_to_c (TS_VM *vm, const char *source, const char *unit_name,
                    TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Error derr;
  const char *r;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return NULL;
    }
  /* Parse (no execution); syntax errors fail the compile. */
  ts_error_clear (&derr);
  if (ts_parse_program (source, &prog, &derr) != TS_OK)
    {
      ts_error_set (error, derr.code != TS_OK ? derr.code : TS_ERR_PARSE,
                    derr.system_errno, derr.offset,
                    "termscript parse error: %s",
                    derr.message[0] ? derr.message : "invalid syntax");
      return NULL;
    }
  r = ts_cg_emit (vm, prog, unit_name, false, error);
  ts_node_free (prog);
  return r;
}

#include "../backends/QBE.inc"

const char *
ts_vm_compile_backend (TS_VM *vm, const char *source,
                       const TS_CompileOptions *options, TS_Error *error)
{
  TS_CompileOptions defaults;
  const TS_CompileOptions *opts = options;
  if (!opts)
    {
      ts_compile_options_init (&defaults);
      opts = &defaults;
    }
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return NULL;
    }
  if (opts->struct_size != sizeof *opts
      || opts->version != TS_COMPILE_OPTIONS_VERSION)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "unsupported compile options version");
      return NULL;
    }
  if (opts->flags != 0)
    {
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "unsupported compile flags: 0x%x", opts->flags);
      return NULL;
    }
  if (opts->target && opts->target[0])
    {
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "target overrides are not supported");
      return NULL;
    }
  for (size_t i = 0; i < sizeof opts->reserved / sizeof opts->reserved[0]; i++)
    if (opts->reserved[i] != 0)
      {
        ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                      "unsupported compile option reserved field %zu", i);
        return NULL;
      }
  switch (opts->backend)
    {
    case TS_BACKEND_C:
      return ts_vm_compile_to_c (vm, source, opts->unit_name, error);
    case TS_BACKEND_QBE:
      return ts_vm_compile_to_qbe (vm, source, false, error);
    case TS_BACKEND_JITERATI:
      ts_error_set (
          error, TS_ERR_UNSUPPORTED, 0, 0,
          "Jiterati is an execution/JIT backend; use ts_vm_run_backend");
      return NULL;
    default:
      ts_error_set (error, TS_ERR_UNSUPPORTED, 0, 0,
                    "unknown Termscript backend: %d", (int)opts->backend);
      return NULL;
    }
}

/* Termlib-flavored compile on a standalone VM (shared emitter).  Used
 * by dt_termscript_compile_to_c via the inner TS VM. */
const char *
ts_vm_compile_to_c_dt (TS_VM *vm, const char *source, const char *unit_name,
                       TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Error derr;
  const char *r;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return NULL;
    }
  ts_error_clear (&derr);
  if (ts_parse_program (source, &prog, &derr) != TS_OK)
    {
      ts_error_set (error, derr.code != TS_OK ? derr.code : TS_ERR_PARSE,
                    derr.system_errno, derr.offset,
                    "termscript parse error: %s",
                    derr.message[0] ? derr.message : "invalid syntax");
      return NULL;
    }
  r = ts_cg_emit (vm, prog, unit_name, true, error);
  ts_node_free (prog);
  return r;
}

const char *
ts_version_string (void)
{
  static char buf[32];
  snprintf (buf, sizeof buf, "%d.%d.%d", TS_VERSION_MAJOR, TS_VERSION_MINOR,
            TS_VERSION_PATCH);
  return buf;
}
