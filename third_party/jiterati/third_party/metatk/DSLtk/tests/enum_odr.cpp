#include "enum_fixtures.hpp"

const Color *
other_translation_unit_values ()
{
  return dsl::enum_values<Color> ().data ();
}

std::string_view
other_translation_unit_name ()
{
  return dsl::enum_name (Color::Red);
}
