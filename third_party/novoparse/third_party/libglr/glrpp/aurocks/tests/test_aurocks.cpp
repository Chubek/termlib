#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>

#include "../../aurocks/Aurocks.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{

std::string
read_file (const std::string &path)
{
  std::ifstream input (path, std::ios::binary);
  REQUIRE (input);
  std::ostringstream buffer;
  buffer << input.rdbuf ();
  return buffer.str ();
}

std::string
json_g_path ()
{
  return std::string (AUROCKS_TEST_DIR) + "/examples/JSON.g";
}

aurocks::GrammarSpec
parse_json_g ()
{
  auto spec = aurocks::SpecParser::parse_file (json_g_path ());
  REQUIRE (spec.is_ok ());
  return spec.unwrap ();
}

aurocks::BuiltGrammar
lower_json_g (const aurocks::GrammarSpec &spec)
{
  auto built = aurocks::GrammarBuilder::lower (spec);
  REQUIRE (built.is_ok ());
  return built.unwrap ();
}

struct LiveParser
{
  std::unique_ptr<glrpp::Grammar> grammar;
  std::unique_ptr<glrpp::Parser> parser;
};

LiveParser
make_live (const aurocks::BuiltGrammar &built)
{
  auto grammar = aurocks::GrammarBuilder::build_grammar (built);
  REQUIRE (grammar.is_ok ());
  LiveParser live;
  live.grammar = grammar.unwrap_move ();
  live.parser = std::make_unique<glrpp::Parser> (*live.grammar);
  aurocks::GrammarBuilder::configure_parser (built, *live.parser);
  return live;
}

bool
parses (LiveParser &live, std::string_view input)
{
  return live.parser->try_parse (input).is_ok ();
}

} // namespace

TEST_CASE ("spec parser reads JSON.g header and directives")
{
  auto spec = parse_json_g ();
  REQUIRE (spec.name == "JSON");
  REQUIRE (spec.directive_value ("start") == "JSON");
  REQUIRE (spec.directive_value ("language") == "C++");
  REQUIRE (spec.directive_value ("parser") == "JSONParser.cpp");
  REQUIRE (spec.directive_value ("runtime") == "JSONParserRuntime.hpp");
  REQUIRE (spec.directive_value ("ast") == "JSONParserAST.hpp");
  REQUIRE (spec.directive_value ("build") == "Makefile");
  REQUIRE (spec.directive_value ("atn") == "on");
  REQUIRE (spec.directive_value ("lexer") == "SCANNERLESS");
}

TEST_CASE ("spec parser reads all JSON.g rules")
{
  auto spec = parse_json_g ();
  REQUIRE (spec.rules.size () == 17);
  REQUIRE (spec.has_rule ("JSON"));
  REQUIRE (spec.has_rule ("Object"));
  REQUIRE (spec.has_rule ("Members"));
  REQUIRE (spec.has_rule ("Member"));
  REQUIRE (spec.has_rule ("Array"));
  REQUIRE (spec.has_rule ("Elements"));
  REQUIRE (spec.has_rule ("String"));
  REQUIRE (spec.has_rule ("Number"));
  REQUIRE (spec.has_rule ("layout"));
  const auto *number = spec.find_rule ("Number");
  REQUIRE (number != nullptr);
  REQUIRE (number->alternatives.size () == 1);
  REQUIRE (number->alternatives.front ().elements.size () == 4);
}

TEST_CASE ("spec parser rejects missing header")
{
  auto spec = aurocks::SpecParser::parse ("A = \"x\"\n");
  REQUIRE (spec.is_err ());
}

TEST_CASE ("spec parser rejects perl-dialect markers")
{
  auto spec = aurocks::SpecParser::parse ("%{\n%} \n%%\nA : \"x\" ;\n%%\n");
  REQUIRE (spec.is_err ());
}

TEST_CASE ("lowering rejects missing start")
{
  auto spec = aurocks::SpecParser::parse ("grammar G\nA = \"x\"\n");
  REQUIRE (spec.is_ok ());
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  REQUIRE (built.is_err ());
}

TEST_CASE ("lowering rejects unknown references")
{
  auto spec
      = aurocks::SpecParser::parse ("grammar G\n%start A\nA = B \"x\"\n");
  REQUIRE (spec.is_ok ());
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  REQUIRE (built.is_err ());
}

TEST_CASE ("lowering rejects duplicate rules")
{
  auto spec = aurocks::SpecParser::parse (
      "grammar G\n%start A\nA = \"x\"\nA = \"y\"\n");
  REQUIRE (spec.is_ok ());
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  REQUIRE (built.is_err ());
}

TEST_CASE ("lowering rejects empty classes")
{
  auto spec
      = aurocks::SpecParser::parse ("grammar G\n%start A\nA = []\n");
  REQUIRE (spec.is_ok ());
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  REQUIRE (built.is_err ());
}

TEST_CASE ("class pattern enumerates ASCII without ranges")
{
  std::string error;
  std::string pattern = aurocks::Emitter::class_pattern ("[0-9]", error);
  REQUIRE (error.empty ());
  REQUIRE (pattern.find ("0|1|2|3|4|5|6|7|8|9") != std::string::npos);
  REQUIRE (pattern.find ('-') == std::string::npos);
}

TEST_CASE ("class pattern rejects partial non-ASCII coverage")
{
  std::string error;
  std::string pattern
      = aurocks::Emitter::class_pattern ("[A-\\u00FF]", error);
  REQUIRE (pattern.empty ());
  REQUIRE (!error.empty ());
}

TEST_CASE ("class pattern covers U+0080..U+FFFF with a negated class")
{
  std::string error;
  std::string pattern
      = aurocks::Emitter::class_pattern ("[\\u0080-\\uFFFF]", error);
  REQUIRE (error.empty ());
  REQUIRE (pattern.rfind ("[^]", 0) == 0);
}

TEST_CASE ("lowering builds layout gaps and a start wrapper")
{
  auto spec = parse_json_g ();
  auto built = lower_json_g (spec);
  REQUIRE (built.has_layout);
  REQUIRE (built.atn);
  REQUIRE (built.scannerless);
  REQUIRE (!built.symbols.empty ());
  REQUIRE (!built.productions.empty ());
  const auto *start = built.find (built.start);
  REQUIRE (start != nullptr);
  REQUIRE (start->name == "JSON$start");
  REQUIRE (built.find_by_name ("layout_star") != nullptr);
  // Every production references valid symbols.
  for (const auto &production : built.productions)
    {
      REQUIRE (built.find (production.lhs) != nullptr);
      for (int id : production.rhs)
        REQUIRE (built.find (id) != nullptr);
    }
}

TEST_CASE ("JSON grammar parses documents end to end")
{
  auto spec = parse_json_g ();
  auto built = lower_json_g (spec);
  auto live = make_live (built);
  REQUIRE (parses (live, "{\"a\": [1, 2.5, true, false, null]}"));
  REQUIRE (parses (live, "  { \"a\" : 1 }  "));
  REQUIRE (parses (live, "42"));
  REQUIRE (parses (live, "[1,[2,[3]]]"));
  REQUIRE (parses (live, "{\"s\": \"a\\\"b\\\\c\\/\\b\\f\\n\\r\\t\\u0041\"}"));
  REQUIRE (parses (live, "{\"u\": \"\xC3\xA9\"}")); // U+00E9 exercises [0080,FFFF]
  REQUIRE_FALSE (parses (live, "{\"a\":}"));
  REQUIRE (parses (live, "{")); // JSON.g: bare "{" is an empty Object
  REQUIRE_FALSE (parses (live, "{\"a\": 1"));
  REQUIRE_FALSE (parses (live, "[1, 2"));
  REQUIRE_FALSE (parses (live, "- 5")); // no gaps inside lexical Number
  REQUIRE_FALSE (parses (live, "{\"a\": 01}"));
}

TEST_CASE ("simple JSON has a single accepted reading")
{
  auto spec = parse_json_g ();
  auto built = lower_json_g (spec);
  auto live = make_live (built);
  auto result = live.parser->try_parse ("{\"a\": 1}");
  REQUIRE (result.is_ok ());
  // Exactly one stack survives: layout gaps and lexical classes admit no
  // competing accepted derivation. (The forest itself retains explored
  // prefixes, so ParseTree::is_ambiguous is not the right probe here.)
  REQUIRE (glr_parser_stack_count (live.parser->handle ()) == 1);
}

TEST_CASE ("emitter writes all four artifacts")
{
  auto spec = parse_json_g ();
  auto built = lower_json_g (spec);
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path () / "aurocks_test_emit";
  fs::remove_all (dir);
  aurocks::EmitOptions options;
  options.out_dir = dir.string ();
  auto emitted = aurocks::Emitter::emit (spec, built, options);
  REQUIRE (emitted.is_ok ());
  REQUIRE (emitted.unwrap ().size () == 4);
  std::string parser = read_file ((dir / "JSONParser.cpp").string ());
  std::string runtime = read_file ((dir / "JSONParserRuntime.hpp").string ());
  std::string ast = read_file ((dir / "JSONParserAST.hpp").string ());
  std::string build = read_file ((dir / "Makefile").string ());
  REQUIRE (parser.find ("build_JSON_grammar") != std::string::npos);
  REQUIRE (parser.find ("AUROCKS_DEMO_MAIN") != std::string::npos);
  REQUIRE (runtime.find ("class JSONParser") != std::string::npos);
  REQUIRE (ast.find ("struct AstNode") != std::string::npos);
  REQUIRE (ast.find ("build_ast") != std::string::npos);
  REQUIRE (build.find ("JSONParser") != std::string::npos);
  fs::remove_all (dir);
}

TEST_CASE ("%skip synthesizes a layout rule")
{
  auto spec = aurocks::SpecParser::parse (
      "grammar G\n%start A\n%skip /[ \\t\\n\\r]/\nA = \"x\" A | \"x\"\n");
  REQUIRE (spec.is_ok ());
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  REQUIRE (built.is_ok ());
  REQUIRE (built.unwrap ().has_layout);
}
