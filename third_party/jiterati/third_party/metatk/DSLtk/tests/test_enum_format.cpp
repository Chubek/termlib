#ifdef DSLTK_TEST_FMT
#include <fmt/format.h>
#endif
#include "enum_fixtures.hpp"

int
main ()
{
#if defined(__cpp_lib_format)
  CHECK_ENUM (std::format ("{}", Color::Red) == "Red");
  CHECK_ENUM (std::format ("{}", static_cast<Color> (42)) == "42");
  CHECK_ENUM (std::format ("{}", Permissions::ReadWrite) == "Read|Write");
  CHECK_ENUM (std::format ("{:>5}", Color::Red) == "  Red");
  CHECK_ENUM (std::format ("{}", State::Ready) == "ready-state");
#endif
#ifdef DSLTK_TEST_FMT
  CHECK_ENUM (fmt::format ("{}", Color::Red) == "Red");
  CHECK_ENUM (fmt::format ("{}", static_cast<Color> (42)) == "42");
  CHECK_ENUM (fmt::format ("{}", Permissions::ReadWrite) == "Read|Write");
  CHECK_ENUM (fmt::format ("{:>5}", Color::Red) == "  Red");
  CHECK_ENUM (fmt::format ("{}", State::Ready) == "ready-state");
#endif
}
