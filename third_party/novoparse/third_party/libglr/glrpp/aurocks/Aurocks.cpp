/**
 * @file Aurocks.cpp
 * @brief Implementation of the Aurocks `.g` frontend over GLRpp.
 *
 * Pipeline: scan `.g` text -> @ref GrammarSpec -> reference validation ->
 * lexical fixpoint -> EBNF desugar (synthetic option/repeat/group rules) ->
 * layout-gap interleave -> @ref BuiltGrammar -> live @ref glrpp::Grammar
 * or generated C++ artifacts.
 *
 * Internal errors throw @ref Fail (already formatted); public entry points
 * convert to `dsl::Result<T, std::string>` because `dsl::Result` exposes no
 * error accessor for plumbing payloads across helper boundaries.
 */

#include "Aurocks.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <sys/stat.h>

namespace aurocks
{

namespace
{

struct Fail : std::runtime_error
{
  using std::runtime_error::runtime_error;
};

std::string
format_at (std::string_view source, const Location &loc,
           std::string_view message)
{
  std::ostringstream out;
  out << source << ":" << loc.line << ":" << loc.column << ": " << message;
  return out.str ();
}

[[noreturn]] void
fail_at (std::string_view source, const Location &loc,
         const std::string &message)
{
  throw Fail (format_at (source, loc, message));
}

std::string
lower_copy (std::string_view text)
{
  std::string out (text);
  std::transform (out.begin (), out.end (), out.begin (),
                  [] (unsigned char c) { return (char)std::tolower (c); });
  return out;
}

std::string
trim_copy (std::string_view text)
{
  size_t begin = 0;
  while (begin < text.size () && std::isspace ((unsigned char)text[begin]))
    ++begin;
  size_t end = text.size ();
  while (end > begin && std::isspace ((unsigned char)text[end - 1]))
    --end;
  return std::string (text.substr (begin, end - begin));
}

bool
hex_value (char c, int &out)
{
  if (c >= '0' && c <= '9')
    {
      out = c - '0';
      return true;
    }
  if (c >= 'a' && c <= 'f')
    {
      out = c - 'a' + 10;
      return true;
    }
  if (c >= 'A' && c <= 'F')
    {
      out = c - 'A' + 10;
      return true;
    }
  return false;
}

void
encode_utf8 (int codepoint, std::string &out)
{
  if (codepoint < 0x80)
    {
      out.push_back ((char)codepoint);
    }
  else if (codepoint < 0x800)
    {
      out.push_back ((char)(0xC0 | (codepoint >> 6)));
      out.push_back ((char)(0x80 | (codepoint & 0x3F)));
    }
  else
    {
      out.push_back ((char)(0xE0 | (codepoint >> 12)));
      out.push_back ((char)(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back ((char)(0x80 | (codepoint & 0x3F)));
    }
}

// Decode one escape at raw[i] (raw[i] == '\\'). Appends literal bytes to
// `bytes`, or sets `codepoint` when `for_class`. Returns the index just
// past the escape, or npos with `error` set.
const size_t npos = std::string::npos;

size_t
decode_escape (const std::string &raw, size_t i, std::string &bytes,
               int &codepoint, bool for_class, std::string &error)
{
  if (i + 1 >= raw.size ())
    {
      error = "dangling backslash";
      return npos;
    }
  char c = raw[i + 1];
  auto emit = [&] (int cp) {
    if (for_class)
      codepoint = cp;
    else
      encode_utf8 (cp, bytes);
  };
  switch (c)
    {
    case 'b':
      emit (0x08);
      return i + 2;
    case 'f':
      emit (0x0C);
      return i + 2;
    case 'n':
      emit (0x0A);
      return i + 2;
    case 'r':
      emit (0x0D);
      return i + 2;
    case 't':
      emit (0x09);
      return i + 2;
    case 'v':
      emit (0x0B);
      return i + 2;
    case '0':
      emit (0x00);
      return i + 2;
    case ' ':
      emit (0x20);
      return i + 2;
    case 'x':
      {
        if (i + 3 >= raw.size ())
          {
            error = "truncated \\x escape (want \\xHH)";
            return npos;
          }
        int hi = 0, lo = 0;
        if (!hex_value (raw[i + 2], hi) || !hex_value (raw[i + 3], lo))
          {
            error = "invalid \\x escape (want \\xHH)";
            return npos;
          }
        emit (hi * 16 + lo);
        return i + 4;
      }
    case 'u':
      {
        if (i + 5 >= raw.size ())
          {
            error = "truncated \\u escape (want \\uHHHH)";
            return npos;
          }
        int cp = 0;
        for (int k = 0; k < 4; ++k)
          {
            int v = 0;
            if (!hex_value (raw[i + 2 + k], v))
              {
                error = "invalid \\u escape (want \\uHHHH)";
                return npos;
              }
            cp = cp * 16 + v;
          }
        emit (cp);
        return i + 6;
      }
    default:
      // `\"`, `\'`, `\\`, `\/`, `\-`, `\]`, ... -> the char itself.
      emit ((unsigned char)c);
      return i + 2;
    }
}

// ---------------------------------------------------------------------------
// Spec scanner.
// ---------------------------------------------------------------------------

struct Token
{
  enum class Kind
  {
    Ident,
    Quoted,
    Class,
    Equals,
    Pipe,
    LParen,
    RParen,
    Question,
    Star,
    Plus,
    Directive,
    GrammarKw,
    Eof,
  };
  Kind kind = Kind::Eof;
  std::string text; // Ident: name; Quoted: decoded bytes; Class: raw
                    // source with brackets; Directive: lowercased name
  std::string line_value; // Directive: rest of line, trimmed
  Location location;
};

bool
is_ident_start (char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool
is_ident_char (char c)
{
  return is_ident_start (c) || (c >= '0' && c <= '9');
}

class Scanner
{
public:
  Scanner (std::string_view text, std::string_view source)
      : text_ (text), source_ (source)
  {
  }

  std::vector<Token>
  scan ()
  {
    std::vector<Token> tokens;
    while (pos_ < text_.size ())
      {
        char c = text_[pos_];
        if (c == '\n' || std::isspace ((unsigned char)c))
          {
            advance ();
            continue;
          }
        if (c == '/' && peek (1) == '/')
          {
            while (pos_ < text_.size () && text_[pos_] != '\n')
              advance ();
            continue;
          }
        if (c == '/' && peek (1) == '*')
          {
            Location start = location_;
            advance ();
            advance ();
            bool closed = false;
            while (pos_ < text_.size ())
              {
                if (text_[pos_] == '*' && peek (1) == '/')
                  {
                    advance ();
                    advance ();
                    closed = true;
                    break;
                  }
                advance ();
              }
            if (!closed)
              fail_at (source_, start, "unterminated block comment");
            continue;
          }
        if (c == '%')
          {
            if (pos_ + 1 < text_.size () && is_ident_char (text_[pos_ + 1]))
              {
                Location start = location_;
                advance ();
                size_t name_begin = pos_;
                while (pos_ < text_.size () && is_ident_char (text_[pos_]))
                  advance ();
                std::string name (
                    text_.substr (name_begin, pos_ - name_begin));
                while (pos_ < text_.size () && text_[pos_] != '\n'
                       && std::isspace ((unsigned char)text_[pos_]))
                  advance ();
                size_t value_begin = pos_;
                while (pos_ < text_.size () && text_[pos_] != '\n')
                  advance ();
                Token token;
                token.kind = Token::Kind::Directive;
                token.text = lower_copy (name);
                token.line_value = trim_copy (
                    text_.substr (value_begin, pos_ - value_begin));
                token.location = start;
                tokens.push_back (std::move (token));
                continue;
              }
            fail_at (source_, location_,
                     "'%' directives need a name (e.g. `%start Rule`); "
                     "`%{ ... %}` and `%%` markers belong to the Perl "
                     "aurocks dialect and are not accepted here");
          }
        if (is_ident_start (c))
          {
            Location start = location_;
            size_t begin = pos_;
            while (pos_ < text_.size () && is_ident_char (text_[pos_]))
              advance ();
            std::string word (text_.substr (begin, pos_ - begin));
            Token token;
            token.location = start;
            if (word == "grammar")
              token.kind = Token::Kind::GrammarKw;
            else
              {
                token.kind = Token::Kind::Ident;
                token.text = word;
              }
            tokens.push_back (std::move (token));
            continue;
          }
        if (c == '"' || c == '\'')
          {
            tokens.push_back (scan_quoted ());
            continue;
          }
        if (c == '[')
          {
            tokens.push_back (scan_class ());
            continue;
          }
        Location start = location_;
        Token token;
        token.location = start;
        switch (c)
          {
          case '=':
            token.kind = Token::Kind::Equals;
            break;
          case '|':
            token.kind = Token::Kind::Pipe;
            break;
          case '(':
            token.kind = Token::Kind::LParen;
            break;
          case ')':
            token.kind = Token::Kind::RParen;
            break;
          case '?':
            token.kind = Token::Kind::Question;
            break;
          case '*':
            token.kind = Token::Kind::Star;
            break;
          case '+':
            token.kind = Token::Kind::Plus;
            break;
          default:
            {
              std::string message = "unexpected character '";
              message.push_back (c);
              message.push_back ('\'');
              fail_at (source_, start, message);
            }
          }
        advance ();
        tokens.push_back (std::move (token));
      }
    Token eof;
    eof.kind = Token::Kind::Eof;
    eof.location = location_;
    tokens.push_back (std::move (eof));
    return tokens;
  }

private:
  Token
  scan_quoted ()
  {
    Location start = location_;
    char quote = text_[pos_];
    advance ();
    std::string raw;
    bool closed = false;
    while (pos_ < text_.size ())
      {
        char d = text_[pos_];
        if (d == '\n')
          break;
        if (d == '\\')
          {
            if (pos_ + 1 >= text_.size ())
              break;
            raw.push_back (d);
            advance ();
            raw.push_back (text_[pos_]);
            char e = text_[pos_];
            size_t want = 0;
            if (e == 'u')
              want = 4;
            else if (e == 'x')
              want = 2;
            for (size_t k = 0;
                 k < want && pos_ + 1 < text_.size ()
                 && text_[pos_ + 1] != '\n';
                 ++k)
              {
                advance ();
                raw.push_back (text_[pos_]);
              }
            advance ();
            continue;
          }
        if (d == quote)
          {
            closed = true;
            advance ();
            break;
          }
        raw.push_back (d);
        advance ();
      }
    if (!closed)
      fail_at (source_, start, "unterminated string literal");
    std::string bytes;
    for (size_t i = 0; i < raw.size ();)
      {
        if (raw[i] == '\\')
          {
            std::string error;
            int cp = 0;
            size_t next
                = decode_escape (raw, i, bytes, cp, false, error);
            if (next == npos)
              fail_at (source_, start, error + " in string literal");
            i = next;
          }
        else
          {
            bytes.push_back (raw[i]);
            ++i;
          }
      }
    Token token;
    token.kind = Token::Kind::Quoted;
    token.text = std::move (bytes);
    token.location = start;
    return token;
  }

  Token
  scan_class ()
  {
    Location start = location_;
    std::string raw;
    raw.push_back (text_[pos_]);
    advance ();
    bool closed = false;
    while (pos_ < text_.size ())
      {
        char d = text_[pos_];
        if (d == '\n')
          break;
        raw.push_back (d);
        advance ();
        if (d == '\\')
          {
            if (pos_ >= text_.size ())
              break;
            raw.push_back (text_[pos_]);
            char e = text_[pos_];
            advance ();
            size_t want = 0;
            if (e == 'u')
              want = 4;
            else if (e == 'x')
              want = 2;
            for (size_t k = 0;
                 k < want && pos_ < text_.size () && text_[pos_] != '\n';
                 ++k)
              {
                raw.push_back (text_[pos_]);
                advance ();
              }
            continue;
          }
        if (d == ']')
          {
            closed = true;
            break;
          }
      }
    if (!closed)
      fail_at (source_, start, "unterminated character class");
    Token token;
    token.kind = Token::Kind::Class;
    token.text = std::move (raw);
    token.location = start;
    return token;
  }

  char
  peek (size_t ahead) const
  {
    return pos_ + ahead < text_.size () ? text_[pos_ + ahead] : '\0';
  }
  void
  advance ()
  {
    if (text_[pos_] == '\n')
      {
        ++location_.line;
        location_.column = 1;
      }
    else
      {
        ++location_.column;
      }
    ++pos_;
  }
  std::string_view text_;
  std::string_view source_;
  size_t pos_ = 0;
  Location location_;
};

// ---------------------------------------------------------------------------
// Structural spec parser over the token stream.
// ---------------------------------------------------------------------------

class SpecBuilder
{
public:
  SpecBuilder (std::vector<Token> tokens, std::string_view source)
      : tokens_ (std::move (tokens)), source_ (source)
  {
  }

  GrammarSpec
  build ()
  {
    GrammarSpec spec;
    spec.source_name = std::string (source_);
    if (!match (Token::Kind::GrammarKw))
      fail ("expected `grammar NAME` header");
    if (at ().kind != Token::Kind::Ident)
      fail ("expected grammar name after `grammar`");
    spec.name = at ().text;
    advance ();
    bool done = false;
    while (!done)
      {
        switch (at ().kind)
          {
          case Token::Kind::Eof:
            done = true;
            break;
          case Token::Kind::Directive:
            {
              Directive directive;
              directive.name = at ().text;
              directive.value = at ().line_value;
              directive.location = at ().location;
              spec.directives.push_back (std::move (directive));
              advance ();
              break;
            }
          case Token::Kind::Ident:
            {
              if (peek ().kind != Token::Kind::Equals)
                fail_at (at ().location, "expected `=` after rule name '"
                                             + at ().text + "'");
              Rule rule;
              rule.name = at ().text;
              rule.location = at ().location;
              advance ();
              advance (); // name, '='
              rule.alternatives = parse_alternatives ();
              spec.rules.push_back (std::move (rule));
              break;
            }
          default:
            fail ("expected rule `Name = ...` or `%directive`");
          }
      }
    if (spec.rules.empty ())
      fail ("grammar has no rules");
    return spec;
  }

private:
  std::vector<Alternative>
  parse_alternatives ()
  {
    std::vector<Alternative> alternatives;
    while (true)
      {
        Alternative alternative;
        alternative.location = at ().location;
        // `=` never appears inside an alternative, so `Ident =` (even
        // across lines) unambiguously starts the next rule.
        while (is_element_start (at ().kind)
               && !(at ().kind == Token::Kind::Ident
                    && peek ().kind == Token::Kind::Equals))
          alternative.elements.push_back (parse_element ());
        if (alternative.elements.empty ())
          fail ("expected at least one element in alternative");
        alternatives.push_back (std::move (alternative));
        if (at ().kind == Token::Kind::Pipe)
          {
            advance ();
            continue;
          }
        break;
      }
    return alternatives;
  }

  static bool
  is_element_start (Token::Kind kind)
  {
    return kind == Token::Kind::Ident || kind == Token::Kind::Quoted
           || kind == Token::Kind::Class || kind == Token::Kind::LParen;
  }

  Element
  parse_element ()
  {
    Element element;
    if (at ().kind == Token::Kind::LParen)
      {
        Location start = at ().location;
        advance ();
        auto inner = parse_alternatives ();
        if (at ().kind != Token::Kind::RParen)
          fail_at (start, "expected `)` to close group");
        advance ();
        for (auto &alt : inner)
          element.group_alternatives.push_back (std::move (alt.elements));
        parse_repeat (element);
      }
    else if (at ().kind == Token::Kind::Ident)
      {
        element.kind = Element::Kind::Ref;
        element.text = at ().text;
        advance ();
        parse_repeat (element);
      }
    else if (at ().kind == Token::Kind::Quoted)
      {
        element.kind = Element::Kind::Literal;
        element.text = at ().text;
        if (element.text.empty ())
          fail_at (at ().location,
                   "empty literal; omit the symbol instead");
        advance ();
        parse_repeat (element);
      }
    else if (at ().kind == Token::Kind::Class)
      {
        element.kind = Element::Kind::Class;
        element.text = at ().text;
        advance ();
        parse_repeat (element);
      }
    else
      {
        fail ("expected rule reference, literal, class, or group");
      }
    return element;
  }

  void
  parse_repeat (Element &element)
  {
    switch (at ().kind)
      {
      case Token::Kind::Question:
        element.repeat = Element::Repeat::Optional;
        advance ();
        break;
      case Token::Kind::Star:
        element.repeat = Element::Repeat::Star;
        advance ();
        break;
      case Token::Kind::Plus:
        element.repeat = Element::Repeat::Plus;
        advance ();
        break;
      default:
        break;
      }
  }

  const Token &
  at () const
  {
    return tokens_[pos_];
  }
  const Token &
  peek () const
  {
    return tokens_[std::min (pos_ + 1, tokens_.size () - 1)];
  }
  void
  advance ()
  {
    if (pos_ + 1 < tokens_.size ())
      ++pos_;
  }
  bool
  match (Token::Kind kind)
  {
    if (at ().kind == kind)
      {
        advance ();
        return true;
      }
    return false;
  }
  [[noreturn]] void
  fail (const std::string &message)
  {
    ::aurocks::fail_at (source_, at ().location, message);
  }
  [[noreturn]] void
  fail_at (const Location &loc, const std::string &message)
  {
    aurocks::fail_at (source_, loc, message);
  }

  std::vector<Token> tokens_;
  std::string_view source_;
  size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Character class model and POSIX ERE translation.
//
// Rationale: in UTF-8 locales (notably en_US.UTF-8) glibc collation makes
// ERE ranges over punctuation/space unreliable (`[ -~]` fails to match
// 'A'; ranges ending at DEL fail to compile), and ranges over encoded
// multibyte bytes fail outright. ASCII classes are therefore emitted as
// alternations of single atoms (no ranges, no brackets). A high tail that
// covers exactly U+0080..U+FFFF becomes a range-free negated class over
// enumerated ASCII: `[^]<ascii atoms>-]`.
// ---------------------------------------------------------------------------

struct ClassItem
{
  bool is_range = false;
  int lo = 0;
  int hi = 0;
};

std::vector<ClassItem>
parse_class_items (const std::string &inner, std::string &error)
{
  std::vector<ClassItem> items;
  size_t i = 0;
  auto decode_atom = [&] (int &cp) -> bool {
    if (i >= inner.size ())
      return false;
    if (inner[i] == '\\')
      {
        std::string bytes;
        size_t next
            = decode_escape (inner, i, bytes, cp, true, error);
        if (next == npos)
          return false;
        i = next;
        return true;
      }
    cp = (unsigned char)inner[i];
    ++i;
    return true;
  };
  while (i < inner.size ())
    {
      if (inner[i] == '\\' || inner[i] != '-')
        {
          int first = 0;
          if (!decode_atom (first))
            return {};
          if (i < inner.size () && inner[i] == '-'
              && i + 1 < inner.size () && inner[i + 1] != ']')
            {
              ++i; // '-'
              int second = 0;
              if (!decode_atom (second))
                return {};
              if (second < first)
                {
                  error = "reversed range in character class";
                  return {};
                }
              items.push_back ({ true, first, second });
            }
          else
            {
              items.push_back ({ false, first, first });
            }
          continue;
        }
      // Unescaped '-' (first position or after a range): literal.
      items.push_back ({ false, 0x2D, 0x2D });
      ++i;
    }
  return items;
}

// ERE atom for one ASCII code point outside brackets.
std::string
ere_atom (int cp)
{
  static const std::string meta = "\\|(){}[]^$.?*+";
  char c = (char)cp;
  if (meta.find (c) != std::string::npos)
    return std::string ("\\") + c;
  return std::string (1, c);
}

} // namespace

std::string
Error::format () const
{
  return format_at (source, location, message);
}

const Rule *
GrammarSpec::find_rule (std::string_view name) const noexcept
{
  for (const auto &rule : rules)
    if (rule.name == name)
      return &rule;
  return nullptr;
}

bool
GrammarSpec::has_rule (std::string_view name) const noexcept
{
  return find_rule (name) != nullptr;
}

std::string
GrammarSpec::directive_value (std::string_view name,
                              std::string_view fallback) const
{
  std::string key = lower_copy (name);
  for (const auto &directive : directives)
    if (directive.name == key)
      return directive.value;
  return std::string (fallback);
}

bool
GrammarSpec::has_directive (std::string_view name) const noexcept
{
  std::string key = lower_copy (name);
  for (const auto &directive : directives)
    if (directive.name == key)
      return true;
  return false;
}

SpecParser::Result
SpecParser::parse (std::string_view text, std::string_view source_name)
{
  try
    {
      Scanner scanner (text, source_name);
      SpecBuilder builder (scanner.scan (), source_name);
      return SpecParser::Result::from_ok (builder.build ());
    }
  catch (const Fail &failure)
    {
      return SpecParser::Result::from_err (failure.what ());
    }
}

SpecParser::Result
SpecParser::parse_file (const std::string &path)
{
  std::ifstream input (path, std::ios::binary);
  if (!input)
    return SpecParser::Result::from_err (path + ": cannot open file");
  std::ostringstream buffer;
  buffer << input.rdbuf ();
  return parse (buffer.str (), path);
}

std::string
Emitter::class_pattern (std::string_view class_source, std::string &error)
{
  if (class_source.size () < 2 || class_source.front () != '['
      || class_source.back () != ']')
    {
      error = "malformed character class (want [...])";
      return {};
    }
  std::string inner (class_source.substr (1, class_source.size () - 2));
  auto items = parse_class_items (inner, error);
  if (!error.empty ())
    return {};
  if (items.empty ())
    {
      error = "empty character class";
      return {};
    }
  for (const auto &item : items)
    {
      int bound = item.is_range ? std::max (item.lo, item.hi) : item.lo;
      if (item.lo == 0 || bound == 0)
        {
          error = "NUL byte is not expressible in scannerless patterns";
          return {};
        }
      if (bound > 0xFFFF)
        {
          error = "code point out of range in character class";
          return {};
        }
    }
  // Partition into ASCII atoms and high intervals.
  std::vector<int> ascii;
  std::vector<std::pair<int, int>> high;
  for (const auto &item : items)
    {
      if (item.hi <= 0x7F)
        {
          for (int cp = item.lo; cp <= item.hi; ++cp)
            ascii.push_back (cp);
        }
      else if (item.lo > 0x7F)
        {
          high.emplace_back (item.lo, item.hi);
        }
      else
        {
          for (int cp = item.lo; cp <= 0x7F; ++cp)
            ascii.push_back (cp);
          high.emplace_back (0x80, item.hi);
        }
    }
  if (ascii.size () > 1024)
    {
      error = "character class too large to enumerate";
      return {};
    }
  std::sort (ascii.begin (), ascii.end ());
  ascii.erase (std::unique (ascii.begin (), ascii.end ()), ascii.end ());
  std::string ascii_part;
  for (size_t k = 0; k < ascii.size (); ++k)
    {
      if (k)
        ascii_part.push_back ('|');
      ascii_part += ere_atom (ascii[k]);
    }
  std::string high_part;
  if (!high.empty ())
    {
      std::sort (high.begin (), high.end ());
      // Require exact coverage of U+0080..U+FFFF.
      int cursor = 0x80;
      for (const auto &interval : high)
        {
          if (interval.first > cursor)
            break;
          cursor = std::max (cursor, interval.second + 1);
        }
      if (cursor <= 0xFFFF)
        {
          error = "class ranges over non-ASCII code points must cover "
                  "U+0080..U+FFFF exactly (split the class or enumerate "
                  "ASCII explicitly)";
          return {};
        }
      // Range-free negated class over enumerated ASCII 0x01..0x7F.
      // Order: ']' first (literal), '-', last (literal).
      high_part = "[^]";
      for (int cp = 0x01; cp <= 0x7F; ++cp)
        {
          if (cp == 0x5D || cp == 0x2D)
            continue;
          if (cp == 0x5C)
            {
              high_part += "\\\\";
              continue;
            }
          high_part.push_back ((char)cp);
        }
      high_part += "-]";
    }
  if (!ascii_part.empty () && !high_part.empty ())
    return "(" + ascii_part + "|" + high_part + ")";
  if (!ascii_part.empty ())
    return ascii.size () == 1 ? ascii_part : "(" + ascii_part + ")";
  return high_part;
}

namespace
{

// ---------------------------------------------------------------------------
// Lowering.
// ---------------------------------------------------------------------------

bool
element_lexical_in (const Element &element,
                    const std::unordered_map<std::string, bool> &lex);

struct Lowering
{
  std::string source;
  GrammarBuilder::Options options;
  BuiltGrammar out;
  std::unordered_map<std::string, int> by_name;
  std::unordered_map<std::string, int> lit_terms;
  std::unordered_map<std::string, int> class_terms;
  std::unordered_map<std::string, bool> rule_lex;
  std::unordered_map<int, bool> sym_lex;
  int synth_counter = 0;
  int layout_star = -1;
  bool gaps = false;

  [[noreturn]] void
  fail (const Location &loc, const std::string &message)
  {
    fail_at (source, loc, message);
  }

  int
  intern_nonterm (const std::string &name, bool lexical)
  {
    auto it = by_name.find (name);
    if (it != by_name.end ())
      return it->second;
    int id = (int)out.symbols.size ();
    by_name.emplace (name, id);
    rule_lex.emplace (name, lexical);
    sym_lex.emplace (id, lexical);
    out.symbols.push_back ({ id, name, false, false, {}, false, {} });
    return id;
  }

  int
  intern_literal (const std::string &bytes, const Location &loc)
  {
    auto it = lit_terms.find (bytes);
    if (it != lit_terms.end ())
      return it->second;
    if (bytes.empty ())
      fail (loc, "empty literal; omit the symbol instead");
    if (bytes.find ('\0') != std::string::npos)
      fail (loc, "NUL byte in literal is not supported");
    int id = (int)out.symbols.size ();
    lit_terms.emplace (bytes, id);
    sym_lex.emplace (id, true);
    BuiltSymbol symbol;
    symbol.id = id;
    symbol.name = "lit:" + bytes;
    symbol.terminal = true;
    symbol.has_literal = true;
    symbol.literal = bytes;
    out.symbols.push_back (std::move (symbol));
    return id;
  }

  int
  intern_class (const std::string &raw, const Location &loc)
  {
    auto it = class_terms.find (raw);
    if (it != class_terms.end ())
      return it->second;
    std::string error;
    std::string pattern = Emitter::class_pattern (raw, error);
    if (!error.empty ())
      fail (loc, error + " in class " + raw);
    int id = (int)out.symbols.size ();
    class_terms.emplace (raw, id);
    sym_lex.emplace (id, true);
    BuiltSymbol symbol;
    symbol.id = id;
    symbol.name = "class:" + raw;
    symbol.terminal = true;
    symbol.has_pattern = true;
    symbol.pattern = std::move (pattern);
    out.symbols.push_back (std::move (symbol));
    return id;
  }

  void
  add_production (const std::string &lhs_name, int lhs,
                  std::vector<int> rhs, const std::string &provenance)
  {
    if (gaps && !rule_lex[lhs_name] && rhs.size () >= 2)
      {
        std::vector<int> widened;
        widened.reserve (rhs.size () * 2 - 1);
        for (size_t k = 0; k < rhs.size (); ++k)
          {
            if (k)
              widened.push_back (layout_star);
            widened.push_back (rhs[k]);
          }
        rhs = std::move (widened);
      }
    int id = (int)out.productions.size ();
    out.productions.push_back ({ id, lhs, std::move (rhs), provenance });
  }

  // Lexicality of one element: literals/classes are lexical, references
  // consult the fixpoint, groups recurse.
  bool
  element_lexical (const Element &element) const
  {
    return element_lexical_in (element, rule_lex);
  }

  bool
  sequence_lexical (const std::vector<Element> &sequence) const
  {
    for (const auto &element : sequence)
      if (!element_lexical (element))
        return false;
    return true;
  }

  int
  lower_element (const std::string &context, const Element &element,
                 const Location &loc)
  {
    int base = -1;
    bool base_lex = true;
    if (!element.is_group ())
      {
        switch (element.kind)
          {
          case Element::Kind::Ref:
            {
              auto it = by_name.find (element.text);
              if (it == by_name.end ())
                fail (loc, "unknown rule '" + element.text + "'");
              base = it->second;
              base_lex = sym_lex[base];
              break;
            }
          case Element::Kind::Literal:
            base = intern_literal (element.text, loc);
            break;
          case Element::Kind::Class:
            base = intern_class (element.text, loc);
            break;
          }
      }
    else
      {
        std::string name
            = context + "$g" + std::to_string (synth_counter++);
        base_lex = true;
        for (const auto &alt : element.group_alternatives)
          if (!sequence_lexical (alt))
            {
              base_lex = false;
              break;
            }
        base = intern_nonterm (name, base_lex);
        size_t index = 0;
        for (const auto &alt : element.group_alternatives)
          {
            std::vector<int> rhs;
            rhs.reserve (alt.size ());
            for (const auto &member : alt)
              rhs.push_back (lower_element (name, member, loc));
            std::ostringstream provenance;
            provenance << name << ":alt" << index++;
            add_production (name, base, std::move (rhs),
                            provenance.str ());
          }
      }
    switch (element.repeat)
      {
      case Element::Repeat::One:
        return base;
      case Element::Repeat::Optional:
        {
          std::string name
              = context + "$opt" + std::to_string (synth_counter++);
          int id = intern_nonterm (name, base_lex);
          add_production (name, id, { base }, name + ":some");
          add_production (name, id, {}, name + ":empty");
          return id;
        }
      case Element::Repeat::Star:
        {
          std::string name
              = context + "$star" + std::to_string (synth_counter++);
          int id = intern_nonterm (name, base_lex);
          // `Star ::= Star base | %empty`: unique derivations without
          // the duplicate single-base ambiguity of a `| base` alternative.
          add_production (name, id, { id, base }, name + ":more");
          add_production (name, id, {}, name + ":empty");
          return id;
        }
      case Element::Repeat::Plus:
        {
          std::string name
              = context + "$plus" + std::to_string (synth_counter++);
          int id = intern_nonterm (name, base_lex);
          add_production (name, id, { id, base }, name + ":more");
          add_production (name, id, { base }, name + ":one");
          return id;
        }
      }
    fail (loc, "unhandled repetition operator");
  }
};

void
check_no_refs (const Element &element, const Location &loc,
               std::string_view source, std::string_view what)
{
  if (element.kind == Element::Kind::Ref && !element.is_group ())
    fail_at (source, loc,
             std::string (what) + " must list terminals only (found "
                 + "reference '" + element.text + "')");
  for (const auto &alt : element.group_alternatives)
    for (const auto &member : alt)
      check_no_refs (member, loc, source, what);
}

void
check_refs (const GrammarSpec &spec, const Rule &rule,
            const std::set<std::string> &names)
{
  std::function<void (const Element &)> visit = [&] (const Element &element) {
    if (element.kind == Element::Kind::Ref && !element.is_group ()
        && !names.count (element.text))
      fail_at (spec.source_name, rule.location,
               "rule '" + rule.name + "' references unknown rule '"
                   + element.text + "'");
    for (const auto &alt : element.group_alternatives)
      for (const auto &member : alt)
        visit (member);
  };
  for (const auto &alt : rule.alternatives)
    for (const auto &element : alt.elements)
      visit (element);
}

bool
element_lexical_in (const Element &element,
                    const std::unordered_map<std::string, bool> &lex)
{
  if (!element.is_group ())
    {
      if (element.kind != Element::Kind::Ref)
        return true;
      auto it = lex.find (element.text);
      return it != lex.end () && it->second;
    }
  for (const auto &alt : element.group_alternatives)
    for (const auto &member : alt)
      if (!element_lexical_in (member, lex))
        return false;
  return true;
}

bool
rule_all_lexical (const Rule &rule,
                  const std::unordered_map<std::string, bool> &lex)
{
  for (const auto &alt : rule.alternatives)
    for (const auto &element : alt.elements)
      if (!element_lexical_in (element, lex))
        return false;
  return true;
}

std::string
first_ident (std::string_view value)
{
  size_t i = 0;
  while (i < value.size () && std::isspace ((unsigned char)value[i]))
    ++i;
  size_t begin = i;
  while (i < value.size ()
         && (std::isalnum ((unsigned char)value[i]) || value[i] == '_'))
    ++i;
  return std::string (value.substr (begin, i - begin));
}

std::string
cpp_quote (const std::string &bytes)
{
  std::string out = "\"";
  for (unsigned char c : bytes)
    {
      switch (c)
        {
        case '\\':
          out += "\\\\";
          break;
        case '"':
          out += "\\\"";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (c >= 0x20 && c <= 0x7E)
            {
              out.push_back ((char)c);
            }
          else
            {
              char buf[8];
              std::snprintf (buf, sizeof buf, "\\%03o", c);
              out += buf;
            }
          break;
        }
    }
  out.push_back ('"');
  return out;
}

std::string
cpp_symbol (int id)
{
  return "s_" + std::to_string (id);
}

std::string
sanitize_ident (std::string_view text, std::string_view fallback)
{
  std::string out;
  for (char c : text)
    out.push_back (std::isalnum ((unsigned char)c) ? c : '_');
  if (out.empty () || std::isdigit ((unsigned char)out.front ()))
    return std::string (fallback);
  return out;
}

bool
make_directories (const std::string &path, std::string &error)
{
  if (path.empty () || path == ".")
    return true;
  std::string current;
  size_t i = 0;
  if (!path.empty () && path[0] == '/')
    {
      current = "/";
      i = 1;
    }
  std::string part;
  auto flush = [&] () -> bool {
    if (part.empty ())
      return true;
    if (current.empty () || current == "/")
      current += part;
    else
      current += "/" + part;
    part.clear ();
    if (current != "/")
      mkdir (current.c_str (), 0755);
    struct stat info;
    if (stat (current.c_str (), &info) != 0 || !S_ISDIR (info.st_mode))
      {
        error = "cannot create directory '" + current + "'";
        return false;
      }
    return true;
  };
  for (; i <= path.size (); ++i)
    {
      if (i == path.size () || path[i] == '/')
        {
          if (!flush ())
            return false;
        }
      else
        {
          part.push_back (path[i]);
        }
    }
  return true;
}

} // namespace

const BuiltSymbol *
BuiltGrammar::find (int id) const noexcept
{
  for (const auto &symbol : symbols)
    if (symbol.id == id)
      return &symbol;
  return nullptr;
}

const BuiltSymbol *
BuiltGrammar::find_by_name (std::string_view name) const noexcept
{
  for (const auto &symbol : symbols)
    if (symbol.name == name)
      return &symbol;
  return nullptr;
}

GrammarBuilder::Result
GrammarBuilder::lower (const GrammarSpec &spec)
{
  return lower (spec, Options ());
}

GrammarBuilder::Result
GrammarBuilder::lower (const GrammarSpec &spec, Options options)
{
  try
    {
      Lowering state;
      state.source = spec.source_name;
      state.options = options;
      state.out.grammar_name = spec.name;
      if (spec.name.empty ())
        state.fail ({}, "grammar has no name");

      std::set<std::string> names;
      for (const auto &rule : spec.rules)
        {
          if (!names.insert (rule.name).second)
            state.fail (rule.location,
                        "duplicate rule '" + rule.name + "'");
          if (rule.name == "layout_star")
            state.fail (rule.location, "rule name 'layout_star' is reserved");
        }

      // Work on a copy so a synthesized layout rule can be appended.
      GrammarSpec owned = spec;
      bool has_layout = owned.has_rule ("layout");
      bool has_skip = owned.has_directive ("skip");
      if (has_layout && has_skip)
        state.fail ({}, "layout rule and %skip both define layout; keep one");
      if (!has_layout && has_skip)
        {
          std::string skip = owned.directive_value ("skip");
          // `/.../  ` with optional trailing flags/words after the regex.
          size_t open = skip.find ('/');
          size_t close = skip.rfind ('/');
          if (open == std::string::npos || close == open)
            state.fail ({}, "%skip needs a /regexp/ value");
          std::string inner = trim_copy (skip.substr (open + 1, close - open - 1));
          if (inner.size () < 2 || inner.front () != '['
              || inner.back () != ']')
            state.fail ({}, "%skip synthesis supports a single [...] class; "
                            "use a `layout` rule for general skips");
          Rule layout;
          layout.name = "layout";
          Alternative alt;
          Element element;
          element.kind = Element::Kind::Class;
          element.text = inner;
          alt.elements.push_back (std::move (element));
          layout.alternatives.push_back (std::move (alt));
          owned.rules.push_back (std::move (layout));
          names.insert ("layout");
          has_layout = true;
        }

      std::string start_name
          = first_ident (owned.directive_value ("start"));
      if (start_name.empty ())
        state.fail ({}, "missing %start directive (e.g. `%start "
                        + (owned.rules.empty () ? std::string ("Rule")
                                                : owned.rules.front ().name)
                        + "`)");
      if (!names.count (start_name))
        state.fail ({}, "%start names unknown rule '" + start_name + "'");

      std::string language = lower_copy (owned.directive_value ("language"));
      if (!language.empty () && language.rfind ("c++", 0) != 0)
        state.fail ({}, "unsupported %language '"
                        + owned.directive_value ("language")
                        + "' (only C++ is supported)");

      bool atn = false;
      std::string atn_value = lower_copy (owned.directive_value ("atn"));
      if (!atn_value.empty ())
        {
          if (atn_value == "on" || atn_value == "true" || atn_value == "1")
            atn = true;
          else if (atn_value == "off" || atn_value == "false"
                   || atn_value == "0")
            atn = false;
          else
            state.fail ({}, "unsupported %ATN value '"
                            + owned.directive_value ("atn")
                            + "' (want on/off)");
        }
      bool scannerless = false;
      std::string lexer = lower_copy (owned.directive_value ("lexer"));
      if (!lexer.empty ())
        {
          if (lexer.find ("scan") != std::string::npos)
            scannerless = true;
          else
            state.fail ({}, "unsupported %lexer value '"
                            + owned.directive_value ("lexer") + "'");
        }

      for (const auto &rule : owned.rules)
        check_refs (owned, rule, names);
      if (has_layout)
        {
          const Rule *layout = owned.find_rule ("layout");
          for (const auto &alt : layout->alternatives)
            for (const auto &element : alt.elements)
              check_no_refs (element, layout->location, state.source,
                             "`layout` rule");
        }

      // Lexical fixpoint: a rule is lexical when it is built only from
      // terminals and lexical rules.
      std::unordered_map<std::string, bool> lex;
      for (const auto &name : names)
        lex.emplace (name, false);
      bool changed = true;
      while (changed)
        {
          changed = false;
          for (const auto &rule : owned.rules)
            {
              if (lex[rule.name])
                continue;
              if (rule_all_lexical (rule, lex))
                {
                  lex[rule.name] = true;
                  changed = true;
                }
            }
        }
      if (has_layout)
        lex["layout"] = true;

      state.gaps = has_layout && options.enable_layout_gaps;
      // Pre-intern every rule so forward references resolve.
      for (const auto &rule : owned.rules)
        state.intern_nonterm (rule.name, lex[rule.name]);
      if (state.gaps)
        {
          state.layout_star = state.intern_nonterm ("layout_star", true);
          state.out.has_layout = true;
        }
      state.out.atn = atn;
      // Scannerless is required as soon as any pattern exists; a bare
      // `%lexer SCANNERLESS` forces it for literal-only grammars.
      state.out.scannerless = scannerless;

      for (const auto &rule : owned.rules)
        {
          int lhs = state.by_name[rule.name];
          size_t index = 0;
          for (const auto &alt : rule.alternatives)
            {
              std::vector<int> rhs;
              rhs.reserve (alt.elements.size ());
              for (const auto &element : alt.elements)
                rhs.push_back (
                    state.lower_element (rule.name, element, rule.location));
              std::ostringstream provenance;
              provenance << rule.name << ":alt" << index++;
              state.add_production (rule.name, lhs, std::move (rhs),
                                    provenance.str ());
            }
        }
      for (const auto &symbol : state.out.symbols)
        if (symbol.has_pattern)
          state.out.scannerless = true;

      if (state.gaps)
        {
          int layout = state.by_name["layout"];
          int star = state.layout_star;
          // `layout_star ::= layout_star layout | %empty`: nullable left
          // recursion keeps derivations unique.
          state.add_production ("layout_star", star, { star, layout },
                                "layout_star:more");
          state.add_production ("layout_star", star, {}, "layout_star:empty");
          int origin = state.by_name[start_name];
          std::string wrapper = start_name + "$start";
          int wrap = state.intern_nonterm (wrapper, true);
          state.add_production (wrapper, wrap, { star, origin, star },
                                wrapper + ":wrap");
          state.out.start = wrap;
        }
      else
        {
          state.out.start = state.by_name[start_name];
        }
      return GrammarBuilder::Result::from_ok (std::move (state.out));
    }
  catch (const Fail &failure)
    {
      return GrammarBuilder::Result::from_err (failure.what ());
    }
}

dsl::Result<std::unique_ptr<glrpp::Grammar>, std::string>
GrammarBuilder::build_grammar (const BuiltGrammar &built)
{
  using Out = dsl::Result<std::unique_ptr<glrpp::Grammar>, std::string>;
  try
    {
      auto grammar = std::make_unique<glrpp::Grammar> ();
      std::unordered_map<int, glrpp::Symbol> symbols;
      for (const auto &symbol : built.symbols)
        {
          glrpp::Symbol created = symbol.terminal
                                      ? grammar->terminal (symbol.name)
                                      : grammar->nonterminal (symbol.name);
          symbols.emplace (symbol.id, std::move (created));
        }
      auto lookup = [&] (int id, const std::string &what) -> glrpp::Symbol {
        auto it = symbols.find (id);
        if (it == symbols.end ())
          throw Fail ("internal error: " + what + " id "
                      + std::to_string (id) + " not interned");
        return it->second;
      };
      for (const auto &production : built.productions)
        {
          glrpp::Symbol lhs = lookup (production.lhs, "production lhs");
          std::vector<glrpp::Symbol> rhs;
          rhs.reserve (production.rhs.size ());
          for (int id : production.rhs)
            rhs.push_back (lookup (id, "production rhs"));
          auto result = grammar->try_add_production (lhs, rhs);
          if (result.is_err ())
            throw Fail ("cannot add production (" + production.provenance
                        + "): grammar rejected the rule body");
        }
      for (const auto &symbol : built.symbols)
        {
          if (symbol.has_pattern)
            {
              auto status = grammar->set_scannerless_pattern (
                  lookup (symbol.id, "pattern terminal"),
                  symbol.pattern);
              if (status.is_err ())
                throw Fail ("cannot set scannerless pattern for '"
                            + symbol.name + "': pattern rejected by libglr");
            }
          if (symbol.has_literal)
            {
              auto status = grammar->set_scannerless_literal (
                  lookup (symbol.id, "literal terminal"), symbol.literal);
              if (status.is_err ())
                throw Fail ("cannot set scannerless literal for '"
                            + symbol.name + "'");
            }
        }
      auto start = lookup (built.start, "start");
      auto status = grammar->try_set_start (start);
      if (status.is_err ())
        throw Fail ("cannot set start symbol");
      return Out::from_ok (std::move (grammar));
    }
  catch (const Fail &failure)
    {
      return Out::from_err (failure.what ());
    }
  catch (const std::exception &caught)
    {
      return Out::from_err (std::string ("cannot build grammar: ")
                            + caught.what ());
    }
}

void
GrammarBuilder::configure_parser (const BuiltGrammar &built,
                                  glrpp::Parser &parser)
{
  if (built.scannerless)
    {
      auto status = parser.set_scannerless (true);
      if (status.is_err ())
        throw std::runtime_error ("aurocks: cannot enable scannerless mode");
    }
  if (built.atn)
    {
      auto status = parser.enable_adaptive_lookahead ();
      if (status.is_err ())
        throw std::runtime_error ("aurocks: cannot enable ATN lookahead");
    }
}

namespace
{

std::string
artifact_names (const GrammarSpec &spec, std::string &parser_base,
                std::string &runtime_base, std::string &ast_base,
                std::string &build_base)
{
  std::string fallback = sanitize_ident (spec.name, "grammar");
  auto named = [&] (std::string_view key, const std::string &want) {
    std::string value = trim_copy (spec.directive_value (key));
    return value.empty () ? want : value;
  };
  parser_base = named ("parser", fallback + "Parser.cpp");
  runtime_base = named ("runtime", fallback + "Runtime.hpp");
  ast_base = named ("ast", fallback + "AST.hpp");
  build_base = named ("build", "Makefile");
  return fallback;
}

std::string
include_guard (std::string_view basename)
{
  std::string out;
  for (char c : basename)
    out.push_back (std::isalnum ((unsigned char)c)
                       ? (char)std::toupper ((unsigned char)c)
                       : '_');
  return out;
}

std::string
provenance_comment (const BuiltProduction &production)
{
  return "// " + production.provenance;
}

} // namespace

std::string
Emitter::parser_cpp (const GrammarSpec &spec, const BuiltGrammar &built)
{
  std::string parser_base, runtime_base, ast_base, build_base;
  std::string fallback
      = artifact_names (spec, parser_base, runtime_base, ast_base, build_base);
  std::string class_name = sanitize_ident (spec.name, "Grammar") + "Parser";
  std::string build_fn = "build_" + sanitize_ident (spec.name, "grammar")
                         + "_grammar";
  const BuiltSymbol *start = built.find (built.start);

  std::ostringstream out;
  out << "// Generated by aurocks (GLRpp) from " << spec.source_name << ".\n";
  out << "// Grammar: " << spec.name;
  if (start)
    out << ", start: " << start->name;
  out << ". Do not edit.\n";
  out << "#include \"" << runtime_base << "\"\n";
  out << "#include <stdexcept>\n\n";
  out << "glrpp::Grammar\n" << build_fn << " ()\n{\n";
  out << "  glrpp::Grammar grammar;\n";
  for (const auto &symbol : built.symbols)
    {
      out << "  auto " << cpp_symbol (symbol.id) << " = grammar."
          << (symbol.terminal ? "terminal" : "nonterminal") << " ("
          << cpp_quote (symbol.name) << ");\n";
    }
  for (const auto &production : built.productions)
    {
      out << "  " << provenance_comment (production) << "\n";
      out << "  {\n";
      out << "    auto result = grammar.try_add_production ("
          << cpp_symbol (production.lhs) << ", {";
      for (size_t k = 0; k < production.rhs.size (); ++k)
        {
          if (k)
            out << ", ";
          out << cpp_symbol (production.rhs[k]);
        }
      out << "});\n";
      out << "    if (result.is_err ())\n";
      out << "      throw std::runtime_error (\"aurocks: production rejected "
             "(\" + std::string ("
          << cpp_quote (production.provenance) << ") + \")\");\n";
      out << "  }\n";
    }
  for (const auto &symbol : built.symbols)
    {
      if (symbol.has_pattern)
        {
          out << "  {\n";
          out << "    auto status = grammar.set_scannerless_pattern ("
              << cpp_symbol (symbol.id) << ", "
              << cpp_quote (symbol.pattern) << ");\n";
          out << "    if (status.is_err ())\n";
          out << "      throw std::runtime_error (\"aurocks: pattern rejected "
                 "for "
              << cpp_symbol (symbol.id) << "\");\n";
          out << "  }\n";
        }
      if (symbol.has_literal)
        {
          out << "  {\n";
          out << "    auto status = grammar.set_scannerless_literal ("
              << cpp_symbol (symbol.id) << ", std::string_view ("
              << cpp_quote (symbol.literal) << ", " << symbol.literal.size ()
              << "));\n";
          out << "    if (status.is_err ())\n";
          out << "      throw std::runtime_error (\"aurocks: literal rejected "
                 "for "
              << cpp_symbol (symbol.id) << "\");\n";
          out << "  }\n";
        }
    }
  out << "  grammar.set_start (" << cpp_symbol (built.start) << ");\n";
  out << "  return grammar;\n}\n\n";
  out << class_name << "::" << class_name << " ()\n";
  out << "    : grammar_ (" << build_fn << " ()), parser_ (grammar_)\n{\n";
  if (built.scannerless)
    out << "  parser_.set_scannerless (true);\n";
  if (built.atn)
    out << "  parser_.enable_adaptive_lookahead ();\n";
  out << "}\n\n";
  out << "dsl::Result<glrpp::ParseTree, std::string>\n";
  out << class_name << "::parse (std::string_view input)\n{\n";
  out << "  return parser_.try_parse (input);\n}\n\n";
  out << "#ifdef AUROCKS_DEMO_MAIN\n";
  out << "#include <fstream>\n#include <iostream>\n#include <sstream>\n";
  out << "#include \"" << ast_base << "\"\n";
  out << "int\nmain (int argc, char **argv)\n{\n";
  out << "  if (argc != 2)\n";
  out << "    {\n";
  out << "      std::cerr << \"usage: \" << argv[0] << \" <input-file>\\n\";\n";
  out << "      return 2;\n";
  out << "    }\n";
  out << "  std::ifstream input (argv[1], std::ios::binary);\n";
  out << "  if (!input)\n";
  out << "    {\n";
  out << "      std::cerr << argv[0] << \": cannot open \" << argv[1] "
         "<< \"\\n\";\n";
  out << "      return 2;\n";
  out << "    }\n";
  out << "  std::ostringstream buffer;\n";
  out << "  buffer << input.rdbuf ();\n";
  out << "  std::string text = buffer.str ();\n";
  out << "  " << class_name << " parser;\n";
  out << "  auto result = parser.parse (text);\n";
  out << "  if (result.is_err ())\n";
  out << "    {\n";
  out << "      std::cout << \"FAIL\\n\";\n";
  out << "      return 1;\n";
  out << "    }\n";
  out << "  auto tree = result.unwrap ();\n";
  out << "  std::cout << to_sexp (build_ast (tree, text, parser.grammar ())) "
         "<< \"\\n\";\n";
  out << "  return 0;\n}\n#endif\n";
  (void)fallback;
  return out.str ();
}

std::string
Emitter::runtime_hpp (const GrammarSpec &spec, const BuiltGrammar &built)
{
  std::string parser_base, runtime_base, ast_base, build_base;
  artifact_names (spec, parser_base, runtime_base, ast_base, build_base);
  std::string class_name = sanitize_ident (spec.name, "Grammar") + "Parser";
  std::string build_fn = "build_" + sanitize_ident (spec.name, "grammar")
                         + "_grammar";

  std::ostringstream out;
  out << "// Generated by aurocks (GLRpp) from " << spec.source_name << ".\n";
  out << "// Runtime wrapper for the " << spec.name << " grammar.\n";
  out << "#ifndef " << include_guard (runtime_base) << "\n";
  out << "#define " << include_guard (runtime_base) << "\n\n";
  out << "#include <GLRpp.hpp>\n\n";
  out << "#include <string_view>\n\n";
  out << "/// Builds the " << spec.name << " grammar.\n";
  out << "glrpp::Grammar " << build_fn << " ();\n\n";
  out << "/// Parser owning its grammar (grammar outlives the engine).\n";
  out << "class " << class_name << "\n{\npublic:\n";
  out << "  " << class_name << " ();\n";
  out << "  dsl::Result<glrpp::ParseTree, std::string> parse "
         "(\n      std::string_view input);\n";
  out << "  const glrpp::Grammar &grammar () const noexcept\n";
  out << "  {\n    return grammar_;\n  }\n";
  out << "  glrpp::Parser &engine () noexcept\n";
  out << "  {\n    return parser_;\n  }\n";
  out << "private:\n";
  out << "  glrpp::Grammar grammar_;\n";
  out << "  glrpp::Parser parser_;\n};\n\n";
  out << "#endif\n";
  (void)built;
  return out.str ();
}

std::string
Emitter::ast_hpp (const GrammarSpec &spec, const BuiltGrammar &built)
{
  std::string parser_base, runtime_base, ast_base, build_base;
  artifact_names (spec, parser_base, runtime_base, ast_base, build_base);

  std::ostringstream out;
  out << "// Generated by aurocks (GLRpp) from " << spec.source_name << ".\n";
  out << "// Untyped AST over the " << spec.name
      << " forest with S-expression output.\n";
  out << "#ifndef " << include_guard (ast_base) << "\n";
  out << "#define " << include_guard (ast_base) << "\n\n";
  out << "#include <GLRpp.hpp>\n";
  out << "#include <cstdio>\n";
  out << "#include <functional>\n";
  out << "#include <string>\n";
  out << "#include <vector>\n\n";
  out << "/// One AST node: named nonterminal/constructor or terminal lexeme.\n";
  out << "struct AstNode\n{\n";
  out << "  std::string kind;\n";
  out << "  std::string lexeme;\n";
  out << "  size_t begin = 0;\n";
  out << "  size_t end = 0;\n";
  out << "  std::vector<AstNode> children;\n};\n\n";
  out << "/// Renders `(kind child... \"lexeme\")`; lexemes are C-escaped.\n";
  out << "inline std::string\nto_sexp (const AstNode &node)\n{\n";
  out << "  std::string out = \"(\";\n";
  out << "  out += node.kind.empty () ? \"?\" : node.kind;\n";
  out << "  for (const auto &child : node.children)\n";
  out << "    {\n";
  out << "      out.push_back (' ');\n";
  out << "      out += to_sexp (child);\n";
  out << "    }\n";
  out << "  if (!node.lexeme.empty ())\n";
  out << "    {\n";
  out << "      out += \" \\\"\";\n";
  out << "      for (unsigned char c : node.lexeme)\n";
  out << "        {\n";
  out << "          if (c == '\\\\' || c == '\\\"')\n";
  out << "            {\n";
  out << "              out.push_back ('\\\\');\n";
  out << "              out.push_back ((char)c);\n";
  out << "            }\n";
  out << "          else if (c >= 0x20 && c <= 0x7E)\n";
  out << "            out.push_back ((char)c);\n";
  out << "          else\n";
  out << "            {\n";
  out << "              char buf[8];\n";
  out << "              std::snprintf (buf, sizeof buf, \"\\\\%03o\", c);\n";
  out << "              out += buf;\n";
  out << "            }\n";
  out << "        }\n";
  out << "      out.push_back ('\\\"');\n";
  out << "    }\n";
  out << "  out.push_back (')');\n";
  out << "  return out;\n}\n\n";
  out << "/// Converts the accepted forest root; ambiguous positions keep\n";
  out << "/// their first derivation.\n";
  out << "inline AstNode\nbuild_ast (const glrpp::ParseTree &tree,\n";
  out << "             std::string_view input, const glrpp::Grammar &grammar)\n{\n";
  out << "  AstNode empty;\n";
  out << "  empty.kind = \"empty\";\n";
  out << "  glr_forest_t *forest = tree.handle ();\n";
  out << "  if (!forest)\n";
  out << "    return empty;\n";
  out << "  std::function<AstNode (glr_forest_node_t *)> convert\n";
  out << "      = [&] (glr_forest_node_t *node) -> AstNode {\n";
  out << "    AstNode out;\n";
  out << "    if (!node)\n";
  out << "      {\n";
  out << "        out.kind = \"null\";\n";
  out << "        return out;\n";
  out << "      }\n";
  out << "    out.begin = node->position;\n";
  out << "    out.end = std::min (node->end_position, input.size ());\n";
  out << "    if (node->type == GLR_NODE_TERMINAL)\n";
  out << "      {\n";
  out << "        const glr_symbol_t *sym\n";
  out << "            = glr_grammar_get_symbol (grammar.handle (),\n";
  out << "                                      node->symbol_id);\n";
  out << "        out.kind = (sym && sym->name) ? sym->name : \"terminal\";\n";
  out << "        if (out.end > out.begin)\n";
  out << "          out.lexeme = std::string (input.substr (out.begin,\n";
  out << "                                                  out.end - out.begin));\n";
  out << "      }\n";
  out << "    else if (node->type == GLR_NODE_CONSTRUCTOR)\n";
  out << "      {\n";
  out << "        const glr_production_t *prod = glr_grammar_get_production (\n";
  out << "            grammar.handle (), node->symbol_id);\n";
  out << "        if (prod && prod->head && prod->head->name)\n";
  out << "          out.kind = std::string (prod->head->name) + \"#\"\n";
  out << "                     + std::to_string (prod->id);\n";
  out << "        else\n";
  out << "          out.kind = \"prod#\" + std::to_string (node->symbol_id);\n";
  out << "        for (size_t i = 0; i < node->child_count; ++i)\n";
  out << "          out.children.push_back (convert (node->children[i]));\n";
  out << "      }\n";
  out << "    else\n";
  out << "      {\n";
  out << "        const glr_symbol_t *sym\n";
  out << "            = glr_grammar_get_symbol (grammar.handle (),\n";
  out << "                                      node->symbol_id);\n";
  out << "        out.kind = (sym && sym->name) ? sym->name : \"nonterminal\";\n";
  out << "        if (node->child_count > 0)\n";
  out << "          out.children.push_back (convert (node->children[0]));\n";
  out << "      }\n";
  out << "    return out;\n";
  out << "  };\n";
  out << "  glr_forest_node_t *entry = forest->root\n";
  out << "                                 ? forest->root\n";
  out << "                                 : (forest->node_count\n";
  out << "                                        ? forest->nodes[0]\n";
  out << "                                        : nullptr);\n";
  out << "  return convert (entry);\n";
  out << "}\n\n";
  out << "#endif\n";
  (void)built;
  return out.str ();
}

std::string
Emitter::makefile (const GrammarSpec &spec, const BuiltGrammar &built,
                   const EmitOptions &options)
{
  std::string parser_base, runtime_base, ast_base, build_base;
  artifact_names (spec, parser_base, runtime_base, ast_base, build_base);
  auto stem = [] (const std::string &base) {
    size_t dot = base.rfind ('.');
    return dot == std::string::npos ? base : base.substr (0, dot);
  };
  std::string demo = stem (parser_base);

  std::ostringstream out;
  out << "# Generated by aurocks (GLRpp) from " << spec.source_name << ".\n";
  out << "# Build tree defaults below; override on the make command line.\n";
  out << "CXX ?= c++\n";
  out << "AUROCKS_CXXFLAGS ?= -std=c++20 -O2";
  if (!options.glrpp_include_dir.empty ())
    out << " -I" << options.glrpp_include_dir;
  if (!options.libglr_include_dir.empty ())
    out << " -I" << options.libglr_include_dir;
  if (!options.extra_includes.empty ())
    out << " " << options.extra_includes;
  out << "\n";
  out << "AUROCKS_LDFLAGS ?=";
  if (!options.libglr_link.empty ())
    out << " " << options.libglr_link;
  else
    out << " -lglr";
  out << "\n\n";
  out << "all: " << demo << "\n\n";
  out << demo << ": " << parser_base << " " << runtime_base << " " << ast_base
      << "\n";
  out << "\t$(CXX) $(AUROCKS_CXXFLAGS) -DAUROCKS_DEMO_MAIN -o " << demo
      << " " << parser_base << " $(AUROCKS_LDFLAGS)\n\n";
  out << "check: " << demo << "\n";
  out << "\t@echo \"run ./" << demo << " <input-file> to parse\"\n\n";
  out << "clean:\n\trm -f " << demo << "\n";
  (void)built;
  return out.str ();
}

Emitter::Result
Emitter::emit (const GrammarSpec &spec, const BuiltGrammar &built,
               const EmitOptions &options)
{
  try
    {
      std::string parser_base, runtime_base, ast_base, build_base;
      artifact_names (spec, parser_base, runtime_base, ast_base, build_base);
      std::string dir = options.out_dir.empty () ? "." : options.out_dir;
      std::string error;
      if (!make_directories (dir, error))
        throw Fail (error);
      auto write = [&] (const std::string &base, const std::string &text,
                        const std::string &kind) {
        std::string path = dir == "." ? base : dir + "/" + base;
        std::ofstream file (path, std::ios::binary | std::ios::trunc);
        if (!file)
          throw Fail ("cannot write '" + path + "'");
        file << text;
        file.flush ();
        if (!file)
          throw Fail ("cannot write '" + path + "'");
        return EmittedFile{ path, kind };
      };
      std::vector<EmittedFile> files;
      files.push_back (
          write (parser_base, parser_cpp (spec, built), "parser"));
      files.push_back (
          write (runtime_base, runtime_hpp (spec, built), "runtime"));
      files.push_back (write (ast_base, ast_hpp (spec, built), "ast"));
      files.push_back (
          write (build_base, makefile (spec, built, options), "build"));
      return Emitter::Result::from_ok (std::move (files));
    }
  catch (const Fail &failure)
    {
      return Emitter::Result::from_err (failure.what ());
    }
}

int
compile_file (const std::string &input_path, const EmitOptions &options,
              std::string &error)
{
  auto spec = SpecParser::parse_file (input_path);
  if (spec.is_err ())
    {
      error = spec.unwrap_err ();
      return 1;
    }
  auto built = GrammarBuilder::lower (spec.unwrap ());
  if (built.is_err ())
    {
      error = built.unwrap_err ();
      return 1;
    }
  GrammarSpec spec_value = spec.unwrap ();
  auto emitted = Emitter::emit (spec_value, built.unwrap (), options);
  if (emitted.is_err ())
    {
      error = emitted.unwrap_err ();
      return 1;
    }
  return 0;
}

} // namespace aurocks
