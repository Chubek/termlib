#include <optional>
#include <string>
#include <string_view>

// Core reflection's custom strings need no container-specific reserve,
// size_type, or stream-extraction interface.
struct TinyString
{
  using value_type = char;
  static constexpr auto npos = std::string_view::npos;
  std::string value;

  TinyString () = default;
  TinyString (const char *text, std::size_t size) : value (text, size) {}
  bool empty () const { return value.empty (); }
  auto begin () const { return value.begin (); }
  auto end () const { return value.end (); }
  void append (std::size_t size, char c) { value.append (size, c); }
  void append (const char *text, std::size_t size) { value.append (text, size); }
};

struct TinyView
{
  using value_type = char;
  static constexpr auto npos = std::string_view::npos;
  std::string_view value;

  constexpr TinyView () = default;
  constexpr TinyView (const char *text) : value (text) {}
  constexpr TinyView (const char *text, std::size_t size) : value (text, size) {}
  constexpr bool empty () const { return value.empty (); }
  constexpr auto size () const { return value.size (); }
  constexpr auto data () const { return value.data (); }
  constexpr auto begin () const { return value.begin (); }
  constexpr auto end () const { return value.end (); }
  constexpr char operator[] (std::size_t i) const { return value[i]; }
  constexpr auto find (char c) const { return value.find (c); }
  constexpr TinyView substr (std::size_t pos, std::size_t size) const
  {
    auto sub = value.substr (pos, size);
    return { sub.data (), sub.size () };
  }
  constexpr void remove_prefix (std::size_t size) { value.remove_prefix (size); }
  constexpr void remove_suffix (std::size_t size) { value.remove_suffix (size); }
  constexpr bool operator== (const TinyView &) const = default;
};

template <typename T> struct TinyOptional : std::optional<T>
{
  using std::optional<T>::optional;
};

#define MAGIC_ENUM_USING_ALIAS_STRING using string = TinyString;
#define MAGIC_ENUM_USING_ALIAS_STRING_VIEW using string_view = TinyView;
#define MAGIC_ENUM_USING_ALIAS_OPTIONAL template <typename T> using optional = TinyOptional<T>;
#include "DSLtk.hpp"

enum class Flag { Read = 1, Write = 2 };

static_assert (std::is_same_v<decltype (dsl::enum_name (Flag::Read)), TinyView>);
static_assert (std::is_same_v<decltype (dsl::enum_cast<Flag> (1)), TinyOptional<Flag>>);
static_assert (dsl::enum_cast<Flag> ("Read") == Flag::Read);
static_assert (dsl::enum_name<Flag::Write> () == TinyView ("Write"));

int
main ()
{
  // Containers not requiring string conversion remain usable with the aliases.
  dsl::containers::bitset<Flag> bits{ Flag::Read };
  return dsl::enum_flags_name (static_cast<Flag> (3)).value == "Read|Write"
         && bits.test (Flag::Read) && !bits.test (Flag::Write) ? 0 : 1;
}
