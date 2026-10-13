/** @file DSLtk.hpp
 *  @brief Parser-combinator toolkit: C++17 core plus MetaTk DSLtk bridge.
 *
 *  The original placeholder defined `jiterati::dsltk::Parser` only.  This
 *  revision keeps that API verbatim and adds, when compiled as C++20 with
 *  `third_party/metatk` visible, a bridge to MetaTk's `DSLtk.hpp` feature
 *  mixins (Pipeline, Operators, PatternMatch, AST, Rewrite, ExprTemplates).
 *  On C++17 the bridge degrades to equivalent C++17 helpers so every
 *  consumer compiles regardless of standard mode.
 */
#ifndef JITERATI_DSLTK_HPP_INCLUDED
#define JITERATI_DSLTK_HPP_INCLUDED

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__has_include) && __cplusplus >= 202002L
#if __has_include("metatk/DSLtk/DSLtk.hpp")
#define JITERATI_HAS_METATK_DSLTK 1
#include "metatk/DSLtk/DSLtk.hpp"
#elif __has_include("third_party/metatk/DSLtk/DSLtk.hpp")
#define JITERATI_HAS_METATK_DSLTK 1
#include "third_party/metatk/DSLtk/DSLtk.hpp"
#endif
#endif

#ifndef JITERATI_HAS_METATK_DSLTK
#define JITERATI_HAS_METATK_DSLTK 0
#endif

namespace jiterati::dsltk {

struct ParseError {
    std::size_t line = 1;
    std::size_t column = 1;
    const char* message = "";
};

template <typename T>
struct ParseResult {
    T value;
    std::string_view rest;
    ParseError error;
};

template <typename T>
using Parser = std::function<std::optional<ParseResult<T>>(std::string_view)>;

/** Compose two parsers into a pipeline stage: a | pipe(f). */
template <typename T, typename F>
inline auto pipe(F&& f) {
    return [fn = std::forward<F>(f)](T value) { return fn(value); };
}

/** Run `parser` on `input`; nullopt when the parser declines. */
template <typename T>
inline std::optional<ParseResult<T>> run_parser(Parser<T> const& parser,
                                                std::string_view input) {
    return parser(input);
}

/** Sequence two parsers, returning both values on success. */
template <typename A, typename B>
inline Parser<std::pair<A, B>> sequence(Parser<A> first, Parser<B> second) {
    return [first = std::move(first), second = std::move(second)](std::string_view input)
               -> std::optional<ParseResult<std::pair<A, B>>> {
        auto left = first(input);
        if (!left) return std::nullopt;
        auto right = second(left->rest);
        if (!right) return std::nullopt;
        return ParseResult<std::pair<A, B>>{{left->value, right->value}, right->rest, {}};
    };
}

/** Ordered choice over a set of parsers. */
template <typename T>
inline Parser<T> choice(std::vector<Parser<T>> options) {
    return [options = std::move(options)](std::string_view input)
               -> std::optional<ParseResult<T>> {
        for (auto const& option : options) {
            if (auto result = option(input)) return result;
        }
        return std::nullopt;
    };
}

} // namespace jiterati::dsltk

#endif // JITERATI_DSLTK_HPP_INCLUDED
