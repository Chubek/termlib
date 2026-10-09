/**
 * @file Aurocks.hpp
 * @brief Aurocks: `.g` grammar frontend lowering to GLRpp/libglr parsers.
 *
 * Aurocks reads a small EBNF grammar specification (see `examples/JSON.g`
 * and `README.md`), lowers it to an explicit symbol/production IR
 * (@ref BuiltGrammar), instantiates it as a @ref glrpp::Grammar, and emits
 * self-contained GLRpp-backed C++ parser artifacts (parser, runtime, AST,
 * Makefile).
 *
 * Dialect sketch:
 * @code
 * grammar JSON
 * %start JSON
 * %language C++
 * %parser JSONParser.cpp
 *
 * JSON = Object | Array | String | Number | "true" | "false" | "null"
 * String = "\"" StringChar* "\""
 * layout = [\ \t\n\r]
 * @endcode
 *
 * Public errors are formatted strings (`path:line:col: message`), matching
 * the @ref glrpp::Grammar `Result<T, std::string>` convention.
 */
#ifndef LIBGLR_GLRPP_AUROCKS_AUROCKS_HPP
#define LIBGLR_GLRPP_AUROCKS_AUROCKS_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../GLRpp.hpp"

namespace aurocks
{

/// Source position (1-based).
struct Location
{
  size_t line = 1;
  size_t column = 1;
};

/// Internal structured diagnostic; rendered with format().
struct Error
{
  std::string source;
  Location location;
  std::string message;
  std::string format () const;
};

/// One EBNF element inside an alternative.
struct Element
{
  enum class Kind
  {
    Ref,    ///< Nonterminal reference: `Member`
    Literal,///< Quoted literal: `"{"`, unescaped to bytes in @ref text
    Class,  ///< Character class: `[0-9]`, raw source (with brackets) in @ref text
  };
  enum class Repeat
  {
    One,
    Optional, ///< `?`
    Star,     ///< `*`
    Plus,     ///< `+`
  };
  Kind kind = Kind::Ref;
  Repeat repeat = Repeat::One;
  std::string text;
  /// Group alternatives (`(a B | c)`); non-empty only for grouped elements.
  /// Group-level repeat applies to the whole group.
  std::vector<std::vector<Element>> group_alternatives;
  bool
  is_group () const noexcept
  {
    return !group_alternatives.empty ();
  }
};

/// One `|`-separated alternative: a sequence of elements.
struct Alternative
{
  std::vector<Element> elements;
  Location location;
};

/// One named rule: `Name = alt | alt ...`.
struct Rule
{
  std::string name;
  std::vector<Alternative> alternatives;
  Location location;
};

/// One `%name value` directive; @ref name is stored lowercased.
struct Directive
{
  std::string name;
  std::string value;
  Location location;
};

/// Parsed `.g` specification.
struct GrammarSpec
{
  std::string name;
  std::string source_name = "<input>";
  std::vector<Directive> directives;
  std::vector<Rule> rules;

  const Rule *find_rule (std::string_view name) const noexcept;
  bool has_rule (std::string_view name) const noexcept;
  /// First directive value with @p name (case-insensitive), or @p fallback.
  std::string directive_value (std::string_view name,
                               std::string_view fallback = {}) const;
  bool has_directive (std::string_view name) const noexcept;
};

/// Parses `.g` source text into a @ref GrammarSpec.
class SpecParser
{
public:
  using Result = dsl::Result<GrammarSpec, std::string>;
  static Result parse (std::string_view text,
                       std::string_view source_name = "<input>");
  static Result parse_file (const std::string &path);
};

/// Fully desugared grammar: explicit symbols and productions.
struct BuiltSymbol
{
  int id = -1;
  std::string name;
  bool terminal = false;
  bool has_pattern = false;
  std::string pattern; ///< POSIX ERE for class terminals (no NUL bytes)
  bool has_literal = false;
  std::string literal; ///< Exact bytes for literal terminals
};

struct BuiltProduction
{
  int id = -1;
  int lhs = -1;
  std::vector<int> rhs;
  std::string provenance; ///< `Rule:alt` or synth description
};

struct BuiltGrammar
{
  std::string grammar_name;
  std::vector<BuiltSymbol> symbols;
  std::vector<BuiltProduction> productions;
  int start = -1;
  bool atn = false;
  bool scannerless = false;
  bool has_layout = false;
  const BuiltSymbol *find (int id) const noexcept;
  const BuiltSymbol *find_by_name (std::string_view name) const noexcept;
};

/// Lowers a @ref GrammarSpec to a @ref BuiltGrammar and to live objects.
class GrammarBuilder
{
public:
  using Result = dsl::Result<BuiltGrammar, std::string>;
  struct Options
  {
    /// Interleave nullable layout gaps between siblings of structural
    /// productions (layout rule required; disable to keep grammar tight).
    bool enable_layout_gaps = true;
  };
  static Result lower (const GrammarSpec &spec);
  static Result lower (const GrammarSpec &spec, Options options);
  /// Instantiate a live GLRpp grammar from lowered IR.
  static dsl::Result<std::unique_ptr<glrpp::Grammar>, std::string>
  build_grammar (const BuiltGrammar &built);
  /// Apply `%lexer`/`%ATN` configuration to a parser over @p built.
  static void configure_parser (const BuiltGrammar &built,
                                glrpp::Parser &parser);
};

/// Emits C++ parser artifacts for a lowered grammar.
struct EmitOptions
{
  std::string out_dir = ".";
  /// Extra `-I` directories (space-separated) for the generated Makefile.
  std::string extra_includes;
  /// Link line fragment for libglr (default: baked build-tree archive).
  std::string libglr_link;
  /// Directory holding `GLRpp.hpp` for the generated `-I` line.
  std::string glrpp_include_dir;
  /// Directory holding `<glr/*.h>` for the generated `-I` line.
  std::string libglr_include_dir;
};

struct EmittedFile
{
  std::string path;
  std::string kind; ///< parser | runtime | ast | build
};

class Emitter
{
public:
  using Result = dsl::Result<std::vector<EmittedFile>, std::string>;
  static Result emit (const GrammarSpec &spec, const BuiltGrammar &built,
                      const EmitOptions &options);
  // Single-artifact generators (also used by tests).
  static std::string parser_cpp (const GrammarSpec &spec,
                                 const BuiltGrammar &built);
  static std::string runtime_hpp (const GrammarSpec &spec,
                                  const BuiltGrammar &built);
  static std::string ast_hpp (const GrammarSpec &spec,
                              const BuiltGrammar &built);
  static std::string makefile (const GrammarSpec &spec,
                               const BuiltGrammar &built,
                               const EmitOptions &options);
  /// Translate `[...]` class source to a POSIX ERE fragment. On failure
  /// returns empty and fills @p error. Public for testing.
  static std::string class_pattern (std::string_view class_source,
                                    std::string &error);
};

/// Parse, lower, and emit in one call. Returns 0 on success, 1 on failure
/// with @p error set to a formatted diagnostic.
int compile_file (const std::string &input_path, const EmitOptions &options,
                  std::string &error);

} // namespace aurocks

#endif
