/**
 * @file cli.cpp
 * @brief `aurocks` command-line driver: check or compile `.g` grammars.
 *
 * Usage: aurocks [--check] [--output-dir DIR] grammar.g
 */

#include "Aurocks.hpp"

#include <iostream>

#ifndef AUROCKS_VERSION
#define AUROCKS_VERSION "1.0.0"
#endif
#ifndef AUROCKS_GLRPP_DIR
#define AUROCKS_GLRPP_DIR ""
#endif
#ifndef AUROCKS_LIBGLR_INCLUDE_DIRS
#define AUROCKS_LIBGLR_INCLUDE_DIRS ""
#endif
#ifndef AUROCKS_LIBGLR_LINK
#define AUROCKS_LIBGLR_LINK "-lglr"
#endif

namespace
{

void
usage (const char *program)
{
  std::cout << "usage: " << program << " [--check] [--output-dir DIR] [--help] [--version] grammar.g\n"
            << "\n"
            << "  --check            validate the grammar and exit\n"
            << "  --output-dir DIR   write generated files to DIR (default: .)\n"
            << "  --help             print this message\n"
            << "  --version          print the aurocks version\n";
}

void
describe (const aurocks::GrammarSpec &spec, const aurocks::BuiltGrammar &built)
{
  size_t terminals = 0;
  for (const auto &symbol : built.symbols)
    if (symbol.terminal)
      ++terminals;
  std::cout << "grammar " << spec.name << ": " << spec.rules.size ()
            << " rules, " << built.symbols.size () << " symbols ("
            << terminals << " terminals), " << built.productions.size ()
            << " productions";
  const aurocks::BuiltSymbol *start = built.find (built.start);
  if (start)
    std::cout << ", start " << start->name;
  std::cout << (built.scannerless ? ", scannerless" : ", tokens")
            << (built.atn ? ", atn" : "") << "\n";
}

} // namespace

int
main (int argc, char **argv)
{
  std::string input;
  std::string out_dir = ".";
  bool check = false;
  for (int i = 1; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--help" || arg == "-h")
        {
          usage (argv[0]);
          return 0;
        }
      if (arg == "--version")
        {
          std::cout << "aurocks " << AUROCKS_VERSION << "\n";
          return 0;
        }
      if (arg == "--check")
        {
          check = true;
        }
      else if (arg == "--output-dir" || arg == "-o")
        {
          if (++i >= argc)
            {
              std::cerr << argv[0] << ": --output-dir needs a directory\n";
              return 2;
            }
          out_dir = argv[i];
        }
      else if (!arg.empty () && arg[0] == '-')
        {
          std::cerr << argv[0] << ": unknown option '" << arg << "'\n";
          return 2;
        }
      else if (!input.empty ())
        {
          std::cerr << argv[0] << ": only one input file is supported\n";
          return 2;
        }
      else
        {
          input = arg;
        }
    }
  if (input.empty ())
    {
      usage (argv[0]);
      return 2;
    }
  auto spec = aurocks::SpecParser::parse_file (input);
  if (spec.is_err ())
    {
      std::cerr << spec.unwrap_err () << "\n";
      return 1;
    }
  auto built = aurocks::GrammarBuilder::lower (spec.unwrap ());
  if (built.is_err ())
    {
      std::cerr << built.unwrap_err () << "\n";
      return 1;
    }
  if (check)
    {
      describe (spec.unwrap (), built.unwrap ());
      return 0;
    }
  aurocks::EmitOptions options;
  options.out_dir = out_dir;
  options.glrpp_include_dir = AUROCKS_GLRPP_DIR;
  options.extra_includes = AUROCKS_LIBGLR_INCLUDE_DIRS;
  options.libglr_link = AUROCKS_LIBGLR_LINK;
  auto emitted
      = aurocks::Emitter::emit (spec.unwrap (), built.unwrap (), options);
  if (emitted.is_err ())
    {
      std::cerr << emitted.unwrap_err () << "\n";
      return 1;
    }
  describe (spec.unwrap (), built.unwrap ());
  for (const auto &file : emitted.unwrap ())
    std::cout << "wrote " << file.path << " (" << file.kind << ")\n";
  return 0;
}
