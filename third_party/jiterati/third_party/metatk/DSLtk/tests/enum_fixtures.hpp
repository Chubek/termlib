#pragma once

#include "DSLtk.hpp"

#include <cstdlib>
#include <iostream>

// Checks remain active in Release builds.
#define CHECK_ENUM(...)                                                        \
  do                                                                          \
    {                                                                         \
      if (!(__VA_ARGS__))                                                     \
        {                                                                     \
          std::cerr << __FILE__ << ':' << __LINE__ << ": " << #__VA_ARGS__      \
                    << '\n';                                                  \
          std::abort ();                                                      \
        }                                                                     \
    }                                                                         \
  while (false)

enum class Color { Red = -10, Green = 0, Blue = 10, Alias = Blue };
enum Direction { Left = 0, Right = 2 };
enum class Boundary : std::int8_t { Low = -128, High = 127 };
enum class Binary : bool { Off = false, On = true };
enum class Empty {};
enum class Large : std::uint64_t { Small = 0, Huge = 1ULL << 50 };
enum class State { Hidden, Ready, Done };
enum class Permissions : std::uint64_t
{
  None = 0,
  Read = 1,
  Write = 4,
  ReadWrite = 5,
  High = 1ULL << 63
};

template <> struct dsl::customize::enum_range<Permissions>
{
  static constexpr bool is_flags = true;
};

template <>
constexpr dsl::customize::customize_t
dsl::customize::enum_name<State> (State value) noexcept
{
  switch (value)
    {
    case State::Hidden: return invalid_tag;
    case State::Ready: return "ready-state";
    default: return default_tag;
    }
}

template <>
constexpr dsl::customize::customize_t
dsl::customize::enum_type_name<State> () noexcept
{
  return "MachineState";
}

namespace nested
{
enum class Token { TOKEN_Start = 1000, TOKEN_Stop = 1002 };

inline auto
magic_enum_define_range_adl (Token)
{
  return dsl::customize::adl_info ().minmax<1000, 1002> ().prefix<6> ();
}
} // namespace nested

struct TokenDSL : dsl::DSL<TokenDSL, dsl::EnumReflection, dsl::PatternMatch> {};

const Color *other_translation_unit_values ();
std::string_view other_translation_unit_name ();
