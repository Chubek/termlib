/* Backend-independent finite binary64 values. Requires POSIX locale APIs. */
TS_Status
ts_value_make_float (TS_Value *v, double number)
{
  if (!v || !isfinite (number))
    return TS_ERR_INVAL;
  ts_value_release (v);
  v->type = TS_FLOAT;
  v->as.real = number;
  return TS_OK;
}

static bool
ts_decimal_spelling (const char *text)
{
  const unsigned char *p = (const unsigned char *)text;
  size_t digits = 0;
  if (!p || strlen (text) > 256)
    return false;
  if (*p == '+' || *p == '-')
    p++;
  while (*p >= '0' && *p <= '9')
    {
      digits++;
      p++;
    }
  if (*p == '.')
    {
      p++;
      while (*p >= '0' && *p <= '9')
        {
          digits++;
          p++;
        }
    }
  if (!digits)
    return false;
  if (*p == 'e' || *p == 'E')
    {
      p++;
      if (*p == '+' || *p == '-')
        p++;
      const unsigned char *exponent = p;
      while (*p >= '0' && *p <= '9')
        p++;
      if (p == exponent)
        return false;
    }
  return *p == 0;
}

TS_Status
ts_value_parse_float (TS_Value *v, const char *text, TS_Error *error)
{
  if (!v || !ts_decimal_spelling (text))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "finite decimal number required (maximum 256 bytes)");
      return TS_ERR_INVAL;
    }
  locale_t numeric = newlocale (LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric)
    {
      ts_error_set (error, TS_ERR_NOMEM, errno, 0,
                    "cannot create numeric locale");
      return TS_ERR_NOMEM;
    }
  errno = 0;
  char *end;
  double number = strtod_l (text, &end, numeric);
  int saved = errno;
  freelocale (numeric);
  if (*end || !isfinite (number) || (saved == ERANGE && number == 0.0))
    {
      ts_error_set (error, TS_ERR_PARSE, saved, 0,
                    "floating-point literal out of range");
      return TS_ERR_PARSE;
    }
  return ts_value_make_float (v, number);
}

static TS_Status
ts_format_float (double number, char *out, size_t size, TS_Error *error)
{
  if (!isfinite (number))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "non-finite floating-point value");
      return TS_ERR_INVAL;
    }
  locale_t numeric = newlocale (LC_NUMERIC_MASK, "C", (locale_t)0);
  if (!numeric)
    {
      ts_error_set (error, TS_ERR_NOMEM, errno, 0,
                    "cannot create numeric locale");
      return TS_ERR_NOMEM;
    }
  locale_t previous = uselocale (numeric);
  if (!previous)
    {
      int saved = errno;
      freelocale (numeric);
      ts_error_set (error, TS_ERR_SYSTEM, saved, 0,
                    "cannot select numeric locale");
      return TS_ERR_SYSTEM;
    }
  int n = snprintf (out, size, "%.17g", number);
  uselocale (previous);
  freelocale (numeric);
  if (n < 0 || (size_t)n + 3 > size)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                    "floating-point formatting limit");
      return TS_ERR_LIMIT;
    }
  if (!strchr (out, '.') && !strchr (out, 'e'))
    strcat (out, ".0");
  return TS_OK;
}

static bool
ts_is_number (const TS_Value *value)
{
  return value->type == TS_INT || value->type == TS_FLOAT;
}

static double
ts_number (const TS_Value *value)
{
  return value->type == TS_FLOAT ? value->as.real : (double)value->as.integer;
}

/* Compare without rounding an int64 to double. The cast is only reached inside
   the exactly checked int64 range; fractional doubles are small enough that
   their truncated integers round-trip exactly. */
static int
ts_compare_int_float (int64_t integer, double real)
{
  if (real < -0x1p63)
    return 1;
  if (real >= 0x1p63)
    return -1;
  int64_t whole = (int64_t)real;
  if (integer < whole)
    return -1;
  if (integer > whole)
    return 1;
  return (double)whole < real ? -1 : (double)whole > real ? 1 : 0;
}

static TS_Status
ts_compare_numbers (const TS_Value *a, const TS_Value *b, int *order,
                    TS_Error *error)
{
  if ((a->type == TS_FLOAT && !isfinite (a->as.real))
      || (b->type == TS_FLOAT && !isfinite (b->as.real)))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "non-finite floating-point operand");
      return TS_ERR_INVAL;
    }
  if (a->type == TS_INT && b->type == TS_INT)
    *order
        = a->as.integer < b->as.integer ? -1 : a->as.integer > b->as.integer;
  else if (a->type == TS_INT)
    *order = ts_compare_int_float (a->as.integer, b->as.real);
  else if (b->type == TS_INT)
    *order = -ts_compare_int_float (b->as.integer, a->as.real);
  else
    *order = a->as.real < b->as.real ? -1 : a->as.real > b->as.real;
  return TS_OK;
}

static TS_Status
ts_float_arith (const TS_Value *argv, TS_Value *ret, TS_Error *error, char op)
{
  double a = ts_number (argv), b = ts_number (argv + 1), result;
  if (!isfinite (a) || !isfinite (b))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "non-finite floating-point operand");
      return TS_ERR_INVAL;
    }
  if ((op == '/' || op == 'm') && b == 0.0)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0,
                    "floating-point division by zero");
      return TS_ERR_PARSE;
    }
  switch (op)
    {
    case '+':
      result = a + b;
      break;
    case '-':
      result = a - b;
      break;
    case '*':
      result = a * b;
      break;
    case '/':
      result = a / b;
      break;
    default:
      result = fmod (a, b);
      break;
    }
  if (!isfinite (result))
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "floating-point overflow");
      return TS_ERR_PARSE;
    }
  return ts_value_make_float (ret, result);
}

static TS_Status
g_float (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void)ud;
  if (ts_check_argc (vm, a, n, 1, e) != TS_OK)
    return TS_ERR_INVAL;
  if (a->type == TS_STRING)
    return ts_value_parse_float (r, a->as.string, e);
  if (ts_is_number (a))
    {
      TS_Status st = ts_value_make_float (r, ts_number (a));
      if (st != TS_OK)
        ts_error_set (e, st, 0, 0, "non-finite floating-point value");
      return st;
    }
  ts_error_set (e, TS_ERR_INVAL, 0, 0,
                "float requires a number or decimal string");
  return TS_ERR_INVAL;
}

static TS_Status
g_int (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void)ud;
  if (ts_check_argc (vm, a, n, 1, e) != TS_OK)
    return TS_ERR_INVAL;
  if (a->type == TS_INT)
    {
      ts_value_make_int (r, a->as.integer);
      return TS_OK;
    }
  if (a->type == TS_FLOAT && isfinite (a->as.real) && a->as.real >= -0x1p63
      && a->as.real < 0x1p63)
    {
      ts_value_make_int (r, (int64_t)a->as.real);
      return TS_OK;
    }
  ts_error_set (e, TS_ERR_INVAL, 0, 0,
                "int requires a finite number in int64 range");
  return TS_ERR_INVAL;
}
