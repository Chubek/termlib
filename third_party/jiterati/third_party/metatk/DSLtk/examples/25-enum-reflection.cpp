#include "DSLtk.hpp"

#include <iostream>

enum class Token { Number = -1, Identifier = 2, End = 7 };
enum class Access { Read = 1, Write = 2, Execute = 4 };

template <> struct dsl::customize::enum_range<Access>
{
  static constexpr bool is_flags = true;
};

struct TokenDSL : dsl::DSL<TokenDSL, dsl::EnumReflection> {};

int
main ()
{
  static_assert (dsl::enum_count<Token> () == 3);
  static_assert (dsl::enum_name<Token::Number> () == "Number");

  for (auto [token, name] : dsl::enum_entries<Token> ())
    std::cout << dsl::enum_integer (token) << ": " << name << '\n';

  auto token = dsl::enum_cast<Token> ("identifier", dsl::case_insensitive);
  if (token)
    dsl::enum_switch ([] (auto constant) {
      constexpr Token value = decltype (constant)::value;
      std::cout << "Dispatched " << dsl::enum_name<value> () << '\n';
    }, *token);

  using namespace dsl::bitwise_operators;
  std::cout << dsl::enum_flags_name (Access::Read | Access::Write) << '\n';

  dsl::containers::array<Token, int> counts{};
  counts[Token::Identifier] = 42;
  std::cout << "Identifiers: " << counts[Token::Identifier] << '\n';
}
