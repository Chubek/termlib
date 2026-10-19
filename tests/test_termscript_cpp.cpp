#include "termscript/include/Termscript.hpp"
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#define CHECK(x) do { if (!(x)) { std::fprintf (stderr, "%d: %s\n", __LINE__, #x); std::abort (); } } while (0)
template<class E, class F> static void raises (F f)
{
  try { f (); } catch (const E &) { return; }
  CHECK (false);
}
static int destroyed;
static void destroy (void *p) { ++destroyed; delete static_cast<int *> (p); }
static TS_Status answer (TS_VM *, void *, const TS_Value *, size_t argc,
                         TS_Value *ret, TS_Error *error)
{
  if (argc)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "answer takes no arguments");
      return TS_ERR_INVAL;
    }
  ts_value_make_int (ret, 42);
  return TS_OK;
}
static const TS_FuncDef functions[] = { { "answer", answer, nullptr } };
static const TS_Module module = TS_MODULE_INIT ("cpp_test", functions);

int main ()
{
  using namespace termscript;
  static_assert (!std::is_copy_constructible_v<vm>);
  static_assert (std::is_nothrow_move_constructible_v<vm>);
  static_assert (std::is_nothrow_move_constructible_v<value>);
  value empty;
  CHECK (empty.type () == TS_NIL && !empty.truthy ());
  value text ("hello");
  CHECK (text.type () == TS_STRING);
  value copy = text;
  CHECK (copy.to_string () == "hello" && copy.get ()->as.string != text.get ()->as.string);
  text = value (42);
  CHECK (text.to_string () == "42" && copy.to_string () == "hello");
  value &self = text;
  text = self;
  CHECK (text.to_string () == "42");
  value moved = std::move (copy);
  CHECK (copy.type () == TS_NIL && moved.to_string () == "hello");
  raises<std::invalid_argument> ([] { value bad (std::string_view ("a\0b", 3)); });
  raises<std::invalid_argument> ([] { value bad (static_cast<const char *> (nullptr)); });
  raises<std::out_of_range> ([] { value bad (UINT64_MAX); });
  CHECK (value (true).to_string () == "true");
  CHECK (value (1.25).type () == TS_FLOAT
         && value (1.25).to_string () == "1.25");
  CHECK (value (-0.0).to_string () == "-0.0" && !value (-0.0).truthy ());
  raises<error> ([] { value bad (std::numeric_limits<double>::infinity ()); });
  static const char tag = 0;
  TS_Value raw{};
  CHECK (ts_value_make_handle (&raw, new int (7), destroy, "test", &tag) == TS_OK);
  {
    auto handle = value::adopt (raw);
    CHECK (raw.type == TS_NIL);
    auto alias = handle;
    auto owner = alias.release ();
    CHECK (alias.type () == TS_NIL && !destroyed);
    ts_value_free (&owner);
    CHECK (!destroyed);
  }
  CHECK (destroyed == 1);

  check_syntax ("G:puts(42);");
  raises<error> ([] { check_syntax ("const =;"); });
  vm machine;
  machine.register_module (module);
  raises<error> ([&] { machine.register_module (module); });
  CHECK (machine.run ("const cpp = G:load(\"cpp_test\"); const n = cpp:answer(); G:puts(n);") == "42\n");
  CHECK (machine.run ("G:puts(42);") == "42\n");
  auto generated = machine.compile ("G:puts(1);");
  CHECK (!generated.empty ());
  machine.compile ("G:puts(2);");
  CHECK (!generated.empty ()); // still owns the earlier output
  raises<std::invalid_argument> ([&] { machine.run (std::string_view ("a\0b", 3)); });
  raises<std::invalid_argument> ([&] { machine.set_args ({ std::string ("a\0b", 3) }); });
  raises<error> ([&] { machine.run_file ("/nonexistent/termscript-wrapper-test"); });
  try
    { machine.run ("G:die(\"expected failure\");"); CHECK (false); }
  catch (const error &e)
    { CHECK (e.code () == TS_ERR_CANCELLED && e.detail ().message[0]); }
  vm next = std::move (machine);
  raises<std::logic_error> ([&] { machine.run ("G:puts(1);"); });
  CHECK (!machine.get () && next.get ());
  CHECK (next.run ("G:puts(3);") == "3\n");
  next.set_args ({ "one", "two" });
  CHECK (next.run ("fn main() { return 7 }").empty ());
  CHECK (next.exit_code () == 7);
  std::puts ("Termscript C++ ownership OK");
}
