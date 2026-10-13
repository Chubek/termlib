#include "enum_fixtures.hpp"
#include "DSLtk.hpp" // Repeated inclusion must be harmless.

#include <sstream>

using namespace dsl::bitwise_operators;

static_assert (dsl::is_magic_enum_supported);
static_assert (dsl::HasFeature<TokenDSL, dsl::EnumReflection>);
static_assert (dsl::is_scoped_enum_v<Color>);
static_assert (!dsl::is_unscoped_enum_v<Color>);
static_assert (dsl::is_unscoped_enum_v<Direction>);
static_assert (!dsl::is_scoped_enum_v<int>);
static_assert (std::is_same_v<dsl::underlying_type_t<Boundary>, std::int8_t>);
static_assert (dsl::is_flags_v<Permissions>);
static_assert (dsl::is_flags_v<const Permissions &>);
static_assert (!dsl::is_flags_v<Color> && !dsl::is_flags_v<int>);
static_assert (dsl::enum_count<Color> () == 3);
static_assert (dsl::enum_count<Empty> () == 0);
static_assert (dsl::enum_count<Boundary> () == 2);
static_assert (dsl::enum_count<Binary> () == 2);
static_assert (dsl::enum_value<Color, 0> () == Color::Red);
static_assert (dsl::enum_value<Color> (2) == Color::Blue);
static_assert (dsl::enum_values<Color> ()[1] == Color::Green);
static_assert (dsl::enum_names<Color> ()[0] == "Red");
static_assert (dsl::enum_entries<Color> ()[1]
               == std::pair{ Color::Green, std::string_view ("Green") });
static_assert (dsl::enum_type_name<Color> () == "Color");
static_assert (dsl::enum_name<Color::Red> () == "Red");
static_assert (dsl::enum_name (Color::Alias) == dsl::enum_name (Color::Blue));
static_assert (dsl::enum_name (static_cast<Color> (1)).empty ());
static_assert (dsl::enum_index<Color::Blue> () == 2);
static_assert (dsl::enum_index (Color::Green) == 1);
static_assert (!dsl::enum_index (static_cast<Color> (1)));
static_assert (dsl::enum_integer (Color::Red) == -10);
static_assert (dsl::enum_underlying (Boundary::High) == 127);
static_assert (dsl::enum_cast<Color> (-10) == Color::Red);
static_assert (dsl::enum_cast<Color> ("Blue") == Color::Blue);
static_assert (dsl::enum_cast<Color> ("bLuE", dsl::case_insensitive) == Color::Blue);
static_assert (!dsl::enum_cast<Color> ("blue"));
static_assert (!dsl::enum_cast<Color> (1));
static_assert (!dsl::enum_cast<Color> (""));
static_assert (!dsl::enum_cast<Color> (std::string_view ("Red\0tail", 8)));
static_assert (dsl::enum_contains (Color::Red));
static_assert (dsl::enum_contains<Color> (-10));
static_assert (dsl::enum_contains<Color> ("green", dsl::case_insensitive));
static_assert (!dsl::enum_contains<Color> ("unknown"));
// Reflected tests the configured interval, whereas contains tests membership.
static_assert (dsl::enum_reflected<Color> (1));
static_assert (!dsl::enum_contains<Color> (1));
static_assert (!dsl::enum_reflected<Color> (500));
static_assert (dsl::enum_count<Large> () == 1);
static_assert (dsl::enum_name<Large::Huge> () == "Huge");
static_assert (dsl::enum_name (Large::Huge).empty ());
static_assert (dsl::enum_type_name<State> () == "MachineState");
static_assert (dsl::enum_count<State> () == 2);
static_assert (!dsl::enum_cast<State> (0));
static_assert (!dsl::enum_cast<State> ("Ready"));
static_assert (dsl::enum_cast<State> ("ready-state") == State::Ready);
static_assert (dsl::enum_name (State::Done) == "Done");
static_assert (dsl::enum_count<nested::Token> () == 2);
static_assert (dsl::enum_name (nested::Token::TOKEN_Start) == "Start");
static_assert (dsl::enum_cast<nested::Token> ("Stop") == nested::Token::TOKEN_Stop);
static_assert (dsl::enum_reflection::enum_count<Color> () == dsl::enum_count<Color> ());

static_assert (dsl::enum_next_value (Color::Red) == Color::Green);
static_assert (!dsl::enum_next_value (Color::Blue));
static_assert (dsl::enum_next_value (Color::Red, 2) == Color::Blue);
static_assert (dsl::enum_prev_value (Color::Blue, 2) == Color::Red);
static_assert (!dsl::enum_prev_value (Color::Red));
static_assert (!dsl::enum_next_value (static_cast<Color> (1)));
static_assert (dsl::enum_next_value_circular (Color::Blue) == Color::Red);
static_assert (dsl::enum_prev_value_circular (Color::Red) == Color::Blue);
static_assert (dsl::enum_next_value_circular (Color::Red, -4) == Color::Blue);

static_assert (dsl::enum_count<Permissions> () == 3);
static_assert (dsl::enum_count<Permissions, dsl::as_common<>> () == 4);
static_assert (dsl::enum_values<Permissions> ()[2] == Permissions::High);
static_assert (dsl::enum_flags_cast<Permissions> (5) == Permissions::ReadWrite);
static_assert (dsl::enum_flags_cast<Permissions> ("Read|Write") == Permissions::ReadWrite);
static_assert (dsl::enum_flags_cast<Permissions> ("read,WRITE", ',', dsl::case_insensitive)
               == Permissions::ReadWrite);
static_assert (!dsl::enum_flags_cast<Permissions> (0));
static_assert (!dsl::enum_flags_cast<Permissions> (2));
static_assert (!dsl::enum_flags_cast<Permissions> ("Read|unknown"));
static_assert (!dsl::enum_flags_cast<Permissions> ("Read||Write"));
static_assert (dsl::enum_flags_contains (Permissions::Read | Permissions::High));
static_assert (dsl::enum_flags_contains<Permissions> (5));
static_assert (dsl::enum_flags_contains<Permissions> ("Read,Write", ','));
static_assert (dsl::enum_flags_test (Permissions::ReadWrite, Permissions::Write));
static_assert (!dsl::enum_flags_test (Permissions::ReadWrite, Permissions::High));
static_assert (!dsl::enum_flags_test (Permissions::ReadWrite, Permissions::None));
static_assert (dsl::enum_flags_test_any (Permissions::ReadWrite,
                                       Permissions::Write | Permissions::High));
static_assert ((Permissions::ReadWrite & ~Permissions::Write) == Permissions::Read);
static_assert ((Permissions::ReadWrite ^ Permissions::Write) == Permissions::Read);

constexpr auto integers = dsl::enum_for_each<Color> (
    [] (auto value) { return dsl::enum_integer (decltype (value)::value); });
static_assert (integers == std::array{ -10, 0, 10 });
constexpr auto constants = dsl::enum_for_each<Color> ([] (auto value) { return value; });
static_assert (std::tuple_size_v<decltype (constants)> == 3);
static_assert (std::get<2> (constants) == Color::Blue);
static_assert (dsl::enum_switch (
                   [] (auto value) { return dsl::enum_integer (decltype (value)::value); },
                   Color::Blue) == 10);
static_assert (dsl::enum_switch (
                   [] (auto value) { return dsl::enum_integer (decltype (value)::value); },
                   static_cast<Color> (1), -999) == -999);
static_assert (dsl::enum_switch (
                   [] (dsl::enum_constant<Color::Blue>) { return 42; },
                   Color::Green, -1) == -1);
static_assert (dsl::enum_fuse (Color::Red, Left) != dsl::enum_fuse (Color::Green, Left));
static_assert (dsl::enum_fuse (Color::Red, Left) != dsl::enum_fuse (Color::Red, Right));

constexpr auto color_array = dsl::containers::make_array<Color> (1, 2, 3);
static_assert (color_array[Color::Blue] == 3);
static_assert (dsl::containers::get<Color::Red> (color_array) == 1);
constexpr dsl::containers::bitset<Color> selected{ Color::Red, Color::Blue };
static_assert (selected.count () == 2);
static_assert (selected.test (Color::Blue) && !selected.test (Color::Green));
constexpr dsl::containers::bitset<Color> raw{ dsl::containers::raw_access, 3U };
static_assert (raw.test (Color::Red) && raw.test (Color::Green));
static_assert (!raw.test (Color::Blue));

int
main ()
{
  CHECK_ENUM (other_translation_unit_values () == dsl::enum_values<Color> ().data ());
  CHECK_ENUM (other_translation_unit_name ().data () == dsl::enum_name (Color::Red).data ());

  CHECK_ENUM (dsl::enum_flags_name (Permissions::Read | Permissions::High) == "Read|High");
  CHECK_ENUM (dsl::enum_flags_name (Permissions::ReadWrite, ',') == "Read,Write");
  CHECK_ENUM (dsl::enum_flags_name (Permissions::None).empty ());
  CHECK_ENUM (dsl::enum_flags_name (static_cast<Permissions> (2)).empty ());
  auto flags = Permissions::Read;
  flags |= Permissions::Write;
  CHECK_ENUM (flags == Permissions::ReadWrite);
  flags &= ~Permissions::Write;
  flags ^= Permissions::High;
  CHECK_ENUM (flags == (Permissions::Read | Permissions::High));

  int visits = 0;
  dsl::enum_for_each<Color> ([&] (auto) { ++visits; });
  CHECK_ENUM (visits == 3);
  dsl::enum_switch ([&] (auto) { ++visits; }, Color::Green);
  CHECK_ENUM (visits == 4);
  dsl::enum_switch ([&] (auto) { ++visits; }, static_cast<Color> (1));
  CHECK_ENUM (visits == 4);

  switch (dsl::enum_fuse (Color::Blue, Right).value ())
    {
    case dsl::enum_fuse (Color::Blue, Right).value (): break;
    default: CHECK_ENUM (false);
    }

  auto array = color_array;
  array[Color::Green] = 42;
  CHECK_ENUM (array.at (Color::Green) == 42);
  auto bits = selected;
  bits.set (Color::Green);
  CHECK_ENUM (bits.all ());
  bits.reset (Color::Red);
  CHECK_ENUM (bits.count () == 2 && bits.any ());
  bits.flip ();
  CHECK_ENUM (bits.count () == 1 && bits.test (Color::Red));
  dsl::containers::set<Color> colors{ Color::Blue, Color::Red };
  CHECK_ENUM (colors.size () == 2 && colors.contains (Color::Red));
  CHECK_ENUM (colors.insert (Color::Green).second);
  CHECK_ENUM (!colors.insert (Color::Green).second);
  CHECK_ENUM (*colors.begin () == Color::Red);
  CHECK_ENUM (colors.erase (Color::Red) == 1);
  CHECK_ENUM (!colors.contains (Color::Red));
  dsl::containers::set<Color, dsl::containers::name_less<>> names{
    Color::Red, Color::Green, Color::Blue
  };
  CHECK_ENUM (*names.begin () == Color::Blue);

  using dsl::iostream_operators::operator<<;
  using dsl::iostream_operators::operator>>;
  std::ostringstream output;
  output << Color::Green << ' ' << static_cast<Color> (42) << ' '
         << Permissions::ReadWrite << ' ' << std::optional{ Color::Blue };
  CHECK_ENUM (output.str () == "Green 42 Read|Write Blue");
  std::istringstream input ("Red unknown");
  Color color = Color::Blue;
  input >> color;
  CHECK_ENUM (!input.fail () && color == Color::Red);
  input >> color;
  CHECK_ENUM (input.fail () && color == Color::Red);
  std::istringstream flag_input ("Read|Write");
  flag_input >> flags;
  CHECK_ENUM (!flag_input.fail () && flags == Permissions::ReadWrite);

  auto table = dsl::match (
      dsl::when<Color::Red> ([] (Color) { return "red"; }),
      dsl::otherwise ([] (Color value) { return dsl::enum_name (value).data (); }));
  CHECK_ENUM (std::string_view (table (Color::Blue)) == "Blue");
}
