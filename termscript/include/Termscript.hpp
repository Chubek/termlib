/* Header-only C++17 ownership wrappers for the standalone C API.
 * Values own strings and retain handle aliases; VM is move-only. All operations
 * require external synchronization on shared objects, exactly as the C API does.
 * Native module descriptors/userdata remain borrowed, not copied by registration.
 * Status failures throw error (including EOF/would-block returned by scripts).
 * C strings are required: embedded NULs are rejected rather than truncated.
 */
#ifndef TERMSCRIPT_HPP
#define TERMSCRIPT_HPP

#include "Termscript.h"
#include <cstdlib>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace termscript
{
class error : public std::runtime_error
{
  TS_Error detail_;
public:
  explicit error (TS_Error detail)
      : std::runtime_error (detail.message[0] ? detail.message
                                              : ts_status_string (detail.code)),
        detail_ (detail) {}
  TS_Status code () const noexcept { return detail_.code; }
  int system_errno () const noexcept { return detail_.system_errno; }
  std::size_t offset () const noexcept { return detail_.offset; }
  const TS_Error &detail () const noexcept { return detail_; }
};

namespace detail
{
inline void check (TS_Status status, TS_Error diagnostic = {})
{
  if (status != TS_OK)
    {
      diagnostic.code = status;
      throw error (diagnostic);
    }
}
inline std::string text (std::string_view s)
{
  if (s.find ('\0') != std::string_view::npos)
    throw std::invalid_argument ("Termscript strings cannot contain NUL");
  return s.empty () ? std::string () : std::string (s);
}
using output_ptr = std::unique_ptr<char, decltype (&std::free)>;
}

class value
{
  TS_Value value_{};
public:
  value () noexcept = default;
  explicit value (bool b) noexcept { ts_value_make_bool (&value_, b); }
  explicit value (double number)
  {
    detail::check (ts_value_make_float (&value_, number));
  }
  template<class I, std::enable_if_t<std::is_integral_v<I>
                                    && !std::is_same_v<I, bool>, int> = 0>
  explicit value (I i)
  {
    if constexpr (std::is_unsigned_v<I>)
      if (i > static_cast<uint64_t> (std::numeric_limits<int64_t>::max ()))
        throw std::out_of_range ("Termscript integer exceeds int64");
    ts_value_make_int (&value_, static_cast<int64_t> (i));
  }
  explicit value (const char *s) : value (s ? std::string_view (s)
                                         : throw std::invalid_argument ("NULL string")) {}
  explicit value (std::string_view s)
  {
    const auto copy = detail::text (s);
    detail::check (ts_value_make_string (&value_, copy.c_str ()));
  }
  /* Copy from a borrowed C value; never steals the caller's payload. */
  explicit value (const TS_Value &v)
  { detail::check (ts_value_copy (&value_, &v)); }
  value (const value &v) : value (v.value_) {}
  value (value &&v) noexcept : value_ (v.value_)
  { v.value_ = TS_Value{}; }
  value &operator= (value v) noexcept { swap (v); return *this; }
  ~value () { ts_value_free (&value_); }
  void swap (value &v) noexcept { std::swap (value_, v.value_); }
  TS_ValueType type () const noexcept { return value_.type; }
  bool truthy () const noexcept { return ts_value_truthy (&value_); }
  /* Borrowed until this value is assigned, moved or destroyed. */
  const TS_Value *get () const noexcept { return &value_; }
  /* Transfers ownership into a C value. */
  TS_Value release () noexcept
  {
    TS_Value v = value_;
    value_ = TS_Value{};
    return v;
  }
  /* Adopts and clears an owning C value; passing a borrowed value is invalid. */
  static value adopt (TS_Value &v) noexcept
  {
    value result;
    result.value_ = v;
    v = TS_Value{};
    return result;
  }
  std::string to_string () const
  {
    TS_Error diagnostic{};
    char *raw = nullptr;
    const auto status = ts_value_to_string (&value_, &raw, &diagnostic);
    detail::output_ptr output (raw, &std::free);
    detail::check (status, diagnostic);
    return output ? output.get () : "";
  }
};

class vm
{
  struct deleter { void operator() (TS_VM *p) const noexcept { ts_vm_free (p); } };
  std::unique_ptr<TS_VM, deleter> vm_;
  TS_VM *require () const
  {
    if (!vm_) throw std::logic_error ("use of moved-from Termscript VM");
    return vm_.get ();
  }
public:
  vm ()
  {
    TS_Error diagnostic{};
    vm_.reset (ts_vm_create (&diagnostic));
    if (!vm_) detail::check (diagnostic.code == TS_OK ? TS_ERR_NOMEM
                                                    : diagnostic.code, diagnostic);
  }
  vm (vm &&) noexcept = default;
  vm &operator= (vm &&) noexcept = default;
  vm (const vm &) = delete;
  vm &operator= (const vm &) = delete;
  TS_VM *get () const noexcept { return vm_.get (); } // borrowed
  void register_module (const TS_Module &module)
  {
    TS_Error diagnostic{};
    auto status = ts_vm_register_module (require (), &module, &diagnostic);
    detail::check (status, diagnostic);
  }
  void add_search_path (std::string_view path)
  {
    auto copy = detail::text (path);
    TS_Error diagnostic{};
    auto status = ts_vm_add_search_path (require (), copy.c_str (), &diagnostic);
    detail::check (status, diagnostic);
  }
  void import (std::string_view name)
  {
    auto copy = detail::text (name);
    TS_Error diagnostic{};
    auto status = ts_vm_import (require (), copy.c_str (), &diagnostic);
    detail::check (status, diagnostic);
  }
  void set_args (const std::vector<std::string> &args)
  {
    std::vector<const char *> pointers;
    pointers.reserve (args.size ());
    for (const auto &arg : args)
      {
        if (arg.find ('\0') != std::string::npos)
          throw std::invalid_argument ("Termscript argument contains NUL");
        pointers.push_back (arg.c_str ());
      }
    TS_Error diagnostic{};
    auto status = ts_vm_set_args (require (), pointers.size (), pointers.data (), &diagnostic);
    detail::check (status, diagnostic);
  }
  std::string run (std::string_view source, TS_Backend backend = TS_BACKEND_VM)
  {
    auto copy = detail::text (source);
    TS_Error diagnostic{};
    char *raw = nullptr;
    auto status = ts_vm_run_backend (require (), copy.c_str (), backend, &raw, &diagnostic);
    detail::output_ptr output (raw, &std::free);
    detail::check (status, diagnostic);
    return output ? output.get () : "";
  }
  std::string run_file (std::string_view path)
  {
    auto copy = detail::text (path);
    TS_Error diagnostic{};
    char *raw = nullptr;
    auto status = ts_vm_run_file (require (), copy.c_str (), &raw, &diagnostic);
    detail::output_ptr output (raw, &std::free);
    detail::check (status, diagnostic);
    return output ? output.get () : "";
  }
  /* Returns an owned copy, unlike the borrowed text returned by the C API. */
  std::string compile (std::string_view source, const TS_CompileOptions *options = nullptr)
  {
    auto copy = detail::text (source);
    TS_Error diagnostic{};
    const char *result = ts_vm_compile_backend (require (), copy.c_str (), options, &diagnostic);
    if (!result) detail::check (diagnostic.code == TS_OK ? TS_ERR_SYSTEM
                                                       : diagnostic.code, diagnostic);
    return result;
  }
  int exit_code () const { return ts_vm_exit_code (require ()); }
};

inline void check_syntax (std::string_view source)
{
  auto copy = detail::text (source);
  TS_Error diagnostic{};
  auto status = ts_check_syntax (copy.c_str (), &diagnostic);
  detail::check (status, diagnostic);
}
} // namespace termscript
#endif
