/* ts_jiterati.cpp -- minimal Termscript/Jiterati execution adapter.
 *
 * The vendored Jiterati executable backend currently exposes a verified AMD64
 * fast path for one IR shape: i32 add(param0, param1).  Keep this adapter tiny
 * and explicit until Jiterati can lower Termscript runtime calls, values and
 * control flow directly.
 */

#include "Jiterati.hpp"
#include "BE/AMD64/Backend.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>

namespace {

enum TsJitOp {
  TS_JIT_ADD = 0,
  TS_JIT_SUB,
  TS_JIT_MUL,
  TS_JIT_DIV,
  TS_JIT_MOD,
  TS_JIT_EQ,
  TS_JIT_LT,
  TS_JIT_GT,
  TS_JIT_LE,
  TS_JIT_GE,
};

void
ts_jit_error (char *buffer, std::size_t size, const char *message)
{
  if (!buffer || size == 0)
    return;
  std::snprintf (buffer, size, "%s", message ? message : "Jiterati error");
}

} // namespace

extern "C" int
ts_jiterati_binary_i32 (int opcode, std::int32_t lhs, std::int32_t rhs,
                        std::int32_t *out, char *error,
                        std::size_t error_size)
{
  if (!out)
    {
      ts_jit_error (error, error_size, "bad Jiterati output pointer");
      return -1;
    }
  try
    {
      jiterati::Module module ("termscript_jiterati");
      jiterati::Function *fn =
        module.create_function<std::int32_t (std::int32_t, std::int32_t)> (
          "ts_binary_i32");
      jiterati::Block *entry = fn->create_block ("entry");
      jiterati::Value lhs_arg = entry->arg (0);
      jiterati::Value rhs_arg = entry->arg (1);
      switch (opcode)
        {
        case TS_JIT_ADD: entry->ret (entry->add (lhs_arg, rhs_arg)); break;
        case TS_JIT_SUB: entry->ret (entry->sub (lhs_arg, rhs_arg)); break;
        case TS_JIT_MUL: entry->ret (entry->mul (lhs_arg, rhs_arg)); break;
        case TS_JIT_DIV: entry->ret (entry->sdiv (lhs_arg, rhs_arg)); break;
        case TS_JIT_MOD: entry->ret (entry->srem (lhs_arg, rhs_arg)); break;
        case TS_JIT_EQ: entry->ret (entry->icmp_eq (lhs_arg, rhs_arg)); break;
        case TS_JIT_LT: entry->ret (entry->icmp_slt (lhs_arg, rhs_arg)); break;
        case TS_JIT_GT: entry->ret (entry->icmp_sgt (lhs_arg, rhs_arg)); break;
        case TS_JIT_LE: entry->ret (entry->icmp_sle (lhs_arg, rhs_arg)); break;
        case TS_JIT_GE: entry->ret (entry->icmp_sge (lhs_arg, rhs_arg)); break;
        default:
          ts_jit_error (error, error_size, "unknown Jiterati i32 opcode");
          return -1;
        }

      jiterati::be::AMD64Backend backend;
      jiterati::JIT jit = jiterati::JIT::create_default ();
      auto compiled = jit.compile (*fn, backend);
      if (!compiled || !compiled->entry ())
        {
          ts_jit_error (error, error_size,
                        "Jiterati AMD64 backend returned no entry point");
          return -1;
        }
      using add_fn = std::int32_t (*) (std::int32_t, std::int32_t);
      add_fn add = reinterpret_cast<add_fn> (compiled->entry ());
      *out = add (lhs, rhs);
      return 0;
    }
  catch (const std::exception &ex)
    {
      ts_jit_error (error, error_size, ex.what ());
      return -1;
    }
  catch (...)
    {
      ts_jit_error (error, error_size, "unknown Jiterati exception");
      return -1;
    }
}

extern "C" int
ts_jiterati_add_i32 (std::int32_t lhs, std::int32_t rhs, std::int32_t *out,
                     char *error, std::size_t error_size)
{
  return ts_jiterati_binary_i32 (TS_JIT_ADD, lhs, rhs, out, error,
                                error_size);
}

extern "C" int
ts_jiterati_available (void)
{
  std::int32_t out = 0;
  char error[128];
  if (ts_jiterati_add_i32 (1, 2, &out, error, sizeof error) != 0)
    return 0;
  return out == 3;
}
