/* shell.cpp -- std.shell: typed LibShell IR and a per-handle POSIX shell.
 * The executor adapter supplies invocation-local streams, so capture also
 * covers parsed lists, loops and functions without modifying stored graphs.
 */
#include "shell/shell.h"
#include "common/ts_std_common.h"

#include "LibShell-Posix.hpp"
#include "lsh-lex.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>

/* The parser is a library consumer in LibShell's cli/ directory, independent
 * of its CLI entry point and embedded-language adapters. */
namespace lsh::cli {
Result<ir::Program> parse_script (std::string_view text);
}

namespace {

constexpr size_t max_text = 1u << 20;
constexpr size_t max_args = 4096;
constexpr size_t max_nodes = 8192;
constexpr size_t max_depth = 128;
constexpr size_t max_output = 64u << 20;
const char shell_tag = 0, word_tag = 0, program_tag = 0;

struct Fault
{
  TS_Status status;
  std::string message;
  int system_errno = 0;
  size_t offset = 0;
};

[[noreturn]] void
fail (TS_Status status, std::string message, int system_errno = 0,
      size_t offset = 0)
{
  throw Fault {status, std::move (message), system_errno, offset};
}

void
check (TS_Status status)
{
  if (status != TS_OK)
    fail (status, "shell: allocation failed", status == TS_ERR_NOMEM ? ENOMEM : 0);
}

void
arity (size_t argc, size_t lo, size_t hi)
{
  if (argc < lo || argc > hi)
    fail (TS_ERR_INVAL, "shell: wrong number of arguments");
}

const char *
text (const TS_Value &value)
{
  if (value.type != TS_STRING)
    fail (TS_ERR_INVAL, "shell: string required");
  const char *s = value.as.string ? value.as.string : "";
  if (std::strlen (s) > max_text)
    fail (TS_ERR_LIMIT, "shell: string exceeds 1 MiB");
  return s;
}

bool
boolean (const TS_Value &value)
{
  if (value.type != TS_BOOL)
    fail (TS_ERR_INVAL, "shell: boolean required");
  return value.as.boolean;
}

int64_t
integer (const TS_Value &value, int64_t lo, int64_t hi)
{
  if (value.type != TS_INT || value.as.integer < lo || value.as.integer > hi)
    fail (TS_ERR_INVAL, "shell: integer out of range");
  return value.as.integer;
}

template <typename T>
T &
handle (const TS_Value &value, const void *tag)
{
  auto *p = static_cast<T *> (ts_value_handle (&value, tag));
  if (!p)
    fail (TS_ERR_INVAL, "shell: wrong handle type");
  return *p;
}

template <typename T>
void
destroy (void *p)
{
  delete static_cast<T *> (p);
}

template <typename T>
TS_Status
wrap (TS_Value *ret, std::unique_ptr<T> p, const char *desc, const void *tag)
{
  check (ts_value_make_handle (ret, p.get (), destroy<T>, desc, tag));
  p.release ();
  return TS_OK;
}

[[noreturn]] void
diagnostic (const lsh::Diagnostic &d, int saved_errno = 0)
{
  TS_Status status;
  switch (d.code)
    {
    case lsh::ErrorCode::not_found: status = TS_ERR_NOTFOUND; break;
    case lsh::ErrorCode::syntax_error:
    case lsh::ErrorCode::bad_arithmetic: status = TS_ERR_PARSE; break;
    case lsh::ErrorCode::timeout: status = TS_ERR_TIMEOUT; break;
    case lsh::ErrorCode::canceled: status = TS_ERR_CANCELLED; break;
    case lsh::ErrorCode::resource_limit: status = TS_ERR_LIMIT; break;
    case lsh::ErrorCode::execution_failed:
    case lsh::ErrorCode::io_error:
    case lsh::ErrorCode::permission_denied: status = TS_ERR_SYSTEM; break;
    default: status = TS_ERR_INVAL; break;
    }
  fail (status, "shell: " + d.message + (d.path.empty () ? "" : " [" + d.path + "]"),
        status == TS_ERR_SYSTEM ? saved_errno : 0);
}

/* Keep the capture bounded even when an external process's pump ignores a
 * Writer diagnostic. Continue draining, then surface the failure to Termscript.
 * This also avoids throwing an allocation exception while a child is live. */
class Capture final : public lsh::Writer
{
public:
  explicit Capture (size_t limit) : limit_ (limit) {}

  lsh::Result<void> write (std::string_view bytes) override
  {
    if (error != TS_OK)
      return lsh::failure (lsh::ErrorCode::resource_limit, {});
    if (bytes.find ('\0') != std::string_view::npos)
      error = TS_ERR_UNSUPPORTED;
    else if (bytes.size () > limit_ - data.size ())
      error = TS_ERR_LIMIT;
    else
      {
        try { data.append (bytes); }
        catch (const std::bad_alloc &) { error = TS_ERR_NOMEM; }
      }
    if (error != TS_OK)
      return lsh::failure (lsh::ErrorCode::resource_limit, {});
    return {};
  }

  std::string data;
  TS_Status error = TS_OK;

private:
  size_t limit_;
};

lsh::ir::NodePtr parse (const char *script);

class Executor final : public lsh::Executor
{
public:
  void bind_runtime (lsh::Environment *env, std::filesystem::path *cwd) override
  { local.bind_runtime (env, cwd); }

  void bind_shell (lsh::Shell *shell) override
  { shell_ = shell; local.bind_shell (shell); }

  lsh::Result<lsh::ExecutionReport> run (const lsh::ExecSpec &spec) override
  { return local.run (bind (spec, true, true)); }

  lsh::Result<lsh::ExecutionReport>
  run_pipeline (std::vector<lsh::ExecSpec> specs, lsh::PipefailPolicy,
                bool merge_stderr) override
  {
    for (size_t i = 0; i < specs.size (); i++)
      specs[i] = bind (std::move (specs[i]), i == 0, i + 1 == specs.size ());
    return local.run_pipeline (std::move (specs), shell_->options ().pipefail,
                               merge_stderr);
  }

  lsh::posix::LocalExecutor local;
  std::shared_ptr<lsh::Reader> input;
  std::shared_ptr<Capture> output, errors;

private:
  lsh::ExecSpec bind (lsh::ExecSpec spec, bool head, bool tail)
  {
    std::vector<lsh::Redirection> defaults;
    if (head && input)
      {
        lsh::Redirection r;
        r.stream = lsh::RedirectStream::stdin_stream;
        r.mode = lsh::RedirectMode::read;
        r.target.kind = lsh::StdioTargetKind::memory;
        r.target.input = input;
        defaults.push_back (std::move (r));
      }
    if (tail && output)
      defaults.push_back (lsh::to_memory (lsh::RedirectStream::stdout_stream, output));
    if (errors)
      defaults.push_back (lsh::to_memory (lsh::RedirectStream::stderr_stream, errors));
    spec.redirections.insert (spec.redirections.begin (), defaults.begin (), defaults.end ());
    return spec;
  }

  lsh::Shell *shell_ = nullptr; /* borrowed from State */
};

struct State
{
  State () : executor (std::make_shared<Executor> ()),
             shell (std::make_unique<lsh::Shell> (executor))
  {
    shell->options ().name = "termscript";
    shell->set_command_substitution_parser ([](std::string_view source) -> lsh::Result<lsh::ir::Program> {
      const std::string copy (source);
      return lsh::ir::Program {parse (copy.c_str ())};
    });
  }

  std::shared_ptr<Executor> executor;
  std::unique_ptr<lsh::Shell> shell;
  lsh::ExecutionReport report;
  std::string output, errors;
  size_t output_limit = max_text;
};

State &
state (const TS_Value &value)
{
  State &s = handle<State> (value, &shell_tag);
  if (!s.shell)
    fail (TS_ERR_INVAL, "shell: closed handle");
  return s;
}

struct Program
{
  lsh::ir::NodePtr root;
};

/* The vendored lexer recurses through nested command substitutions, and the
 * parser through compound commands. Preflight their introducer counts before
 * entering either recursive path; graph validation then enforces actual depth.
 */
void
source_limits (std::string_view source)
{
  if (source.size () > max_text)
    fail (TS_ERR_LIMIT, "shell: source exceeds 1 MiB");
  size_t count = 0, offset = 0;
  while ((offset = source.find ("$(", offset)) != std::string_view::npos)
    {
      if (++count > max_depth)
        fail (TS_ERR_LIMIT, "shell: too many substitution introducers", 0, offset);
      offset += 2;
    }
}

/* Builders cannot create cycles. Bound the tree before LibShell's recursive
 * validator/dispatcher, counting shared subgraphs once per occurrence. */
void
graph_limits (const lsh::ir::NodePtr &node, size_t depth, size_t &count)
{
  if (!node)
    fail (TS_ERR_INVAL, "shell: null program node");
  if (depth > max_depth || ++count > max_nodes)
    fail (TS_ERR_LIMIT, "shell: command graph exceeds depth/node limit");
  auto child = [&](const lsh::ir::NodePtr &n) { graph_limits (n, depth + 1, count); };
  std::visit ([&](const auto &n) {
    using T = std::decay_t<decltype (n)>;
    if constexpr (std::is_same_v<T, lsh::ir::Sequence>)
      { child (n.left); child (n.right); }
    else if constexpr (std::is_same_v<T, lsh::ir::Subshell>
                       || std::is_same_v<T, lsh::ir::BraceGroup>
                       || std::is_same_v<T, lsh::ir::WhileClause>
                       || std::is_same_v<T, lsh::ir::ForClause>
                       || std::is_same_v<T, lsh::ir::FunctionDefinition>)
      {
        if constexpr (std::is_same_v<T, lsh::ir::WhileClause>) child (n.condition);
        child (n.body);
      }
    else if constexpr (std::is_same_v<T, lsh::ir::Redirected>
                       || std::is_same_v<T, lsh::ir::Negate>)
      child (n.subject);
    else if constexpr (std::is_same_v<T, lsh::ir::IfClause>)
      { child (n.condition); child (n.body); if (n.alternative) child (*n.alternative); }
    else if constexpr (std::is_same_v<T, lsh::ir::CaseClause>)
      { for (const auto &item : n.items) child (item.body); }
    else if constexpr (std::is_same_v<T, lsh::ir::Command>)
      { if (n.argv.size () > max_args) fail (TS_ERR_LIMIT, "shell: too many arguments"); }
    else if constexpr (std::is_same_v<T, lsh::ir::Pipeline>)
      {
        if (n.commands.size () > max_args || (count += n.commands.size ()) > max_nodes)
          fail (TS_ERR_LIMIT, "shell: too many pipeline stages");
        for (const auto &cmd : n.commands)
          if (cmd.argv.size () > max_args) fail (TS_ERR_LIMIT, "shell: too many arguments");
      }
  }, node->value);
}

void
validate (const lsh::ir::NodePtr &node)
{
  size_t count = 0;
  graph_limits (node, 1, count);
  auto validation = lsh::ir::validate ({node});
  if (!validation.ok ())
    diagnostic (validation.diagnostics.front ());
}

lsh::ir::NodePtr
parse (const char *script)
{
  source_limits (script);
  {
    lsh::cli::Lexer lexer (script);
    if (lexer.error ()) fail (TS_ERR_PARSE, "shell: " + lexer.error ()->message);
    if (lexer.tokens ().size () > max_nodes)
      fail (TS_ERR_LIMIT, "shell: too many source tokens");
    size_t compounds = 0, lists = 0;
    for (const auto &token : lexer.tokens ())
      {
        const bool opener = token.kind == lsh::cli::TokenKind::operator_token
                              && token.op == lsh::cli::Operator::subshell_open;
        const bool keyword = token.reserved && (token.text == "if" || token.text == "elif"
                              || token.text == "while" || token.text == "until"
                              || token.text == "for" || token.text == "case"
                              || token.text == "function" || token.text == "{");
        if ((opener || keyword) && ++compounds > max_depth)
          fail (TS_ERR_LIMIT, "shell: too many compound-command introducers", 0, token.offset);
        const bool list = token.kind == lsh::cli::TokenKind::operator_token
                           && (token.op == lsh::cli::Operator::semicolon
                               || token.op == lsh::cli::Operator::and_if
                               || token.op == lsh::cli::Operator::or_if);
        if (list && ++lists > max_depth)
          fail (TS_ERR_LIMIT, "shell: too many list connectives", 0, token.offset);
      }
  }
  auto parsed = lsh::cli::parse_script (script);
  if (!parsed)
    fail (TS_ERR_PARSE, "shell: " + parsed.error ().message);
  auto root = std::move (parsed).value ().root;
  validate (root);
  return root;
}

lsh::ir::NodePtr
program (const TS_Value &value)
{
  if (value.type == TS_STRING)
    return parse (text (value));
  return handle<Program> (value, &program_tag).root;
}

TS_Status
program_result (TS_Value *ret, lsh::ir::NodePtr node)
{
  validate (node);
  return wrap (ret, std::make_unique<Program> (Program {std::move (node)}),
               "shell program", &program_tag);
}

lsh::Argument
word (const TS_Value &value)
{
  if (value.type == TS_STRING)
    return lsh::Argument::raw (text (value));
  return handle<lsh::Argument> (value, &word_tag);
}

lsh::Argument
parse_word (const char *s)
{
  source_limits (s);
  lsh::cli::Lexer lexer (s);
  if (lexer.error ())
    fail (TS_ERR_PARSE, "shell: " + lexer.error ()->message);
  const auto &tokens = lexer.tokens ();
  if (tokens.size () != 2 || tokens.front ().kind != lsh::cli::TokenKind::word)
    fail (TS_ERR_INVAL, "shell: word requires exactly one shell word");
  return lsh::Argument {tokens.front ().fragments};
}

template <typename F>
void
items (const TS_Value *argv, size_t argc, F visit)
{
  if (argc == 1 && ts_value_handle (&argv[0], ts_std_list_tag ()))
    {
      auto *list = static_cast<ts_std_list_t *> (ts_value_handle (&argv[0], ts_std_list_tag ()));
      size_t n = ts_std_list_len (list);
      arity (n, 1, max_args);
      for (size_t i = 0; i < n; i++) visit (*ts_std_list_get (list, i));
    }
  else
    {
      arity (argc, 1, max_args);
      for (size_t i = 0; i < argc; i++) visit (argv[i]);
    }
}

TS_Status
s_new (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 0, 1);
  bool inherit = argc == 0 || boolean (argv[0]);
  auto s = std::make_unique<State> ();
  if (!inherit)
    s->shell->env ().clear ();
  s->shell->env ().set ("PWD", s->shell->cwd ().string (), true);
  s->shell->env ().set ("IFS", " \t\n", false);
  return wrap (ret, std::move (s), "shell", &shell_tag);
}

TS_Status
s_close (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  State &s = handle<State> (argv[0], &shell_tag);
  s.shell.reset ();
  s.executor.reset ();
  s.report = {};
  std::string ().swap (s.output);
  std::string ().swap (s.errors);
  ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
s_get (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 3);
  auto found = state (argv[0]).shell->env ().get (text (argv[1]));
  if (found) check (ts_value_make_string (ret, found->c_str ()));
  else if (argc == 3) { ts_value_free (ret); check (ts_value_copy (ret, &argv[2])); }
  else ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
s_set (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 3, 4);
  auto &env = state (argv[0]).shell->env ();
  const char *key = text (argv[1]);
  bool exported = argc == 4 ? boolean (argv[3]) : env.is_exported (key);
  auto assigned = env.assign (key, text (argv[2]), exported);
  if (!assigned) diagnostic (assigned.error ());
  ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
s_unset (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 2);
  auto &env = state (argv[0]).shell->env ();
  const char *key = text (argv[1]);
  if (!lsh::valid_variable_name (key) || env.is_readonly (key))
    fail (TS_ERR_INVAL, "shell: invalid or read-only variable");
  env.unset (key);
  ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
s_export (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 3);
  auto &env = state (argv[0]).shell->env ();
  const char *key = text (argv[1]);
  bool exported = argc == 2 || boolean (argv[2]);
  if (!lsh::valid_variable_name (key))
    fail (TS_ERR_INVAL, "shell: invalid variable name");
  if (!env.contains (key)) env.set (key, "", exported);
  else if (exported) env.export_var (key);
  else env.unexport_var (key);
  ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
s_cwd (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  check (ts_value_make_string (ret, state (argv[0]).shell->cwd ().c_str ()));
  return TS_OK;
}

TS_Status
s_cd (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 2);
  auto &shell = *state (argv[0]).shell;
  std::filesystem::path path = text (argv[1]);
  if (path.empty ()) fail (TS_ERR_INVAL, "shell: empty working directory");
  if (path.is_relative ()) path = shell.cwd () / path;
  std::error_code ec;
  path = std::filesystem::canonical (path, ec);
  if (ec) fail (TS_ERR_SYSTEM, "shell: " + ec.message (), ec.value ());
  if (!std::filesystem::is_directory (path, ec))
    fail (TS_ERR_SYSTEM, "shell: not a directory", ec ? ec.value () : ENOTDIR);
  shell.set_cwd (std::move (path));
  ts_std_ret_nil (ret);
  return TS_OK;
}

using List = std::unique_ptr<ts_std_list_t, decltype (&ts_std_list_free)>;

TS_Status
strings (TS_Value *ret, const std::vector<std::string> &values, TS_Error *error)
{
  List list (ts_std_list_new (), ts_std_list_free);
  if (!list) check (TS_ERR_NOMEM);
  for (const auto &s : values) check (ts_std_list_push_str (list.get (), s.c_str ()));
  /* list_wrap consumes even on failure. */
  return ts_std_list_wrap (ret, list.release (), error);
}

TS_Status
s_args (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *error)
{
  arity (argc, 1, 2);
  auto &shell = *state (argv[0]).shell;
  if (argc == 2)
    {
      auto &list = handle<ts_std_list_t> (argv[1], ts_std_list_tag ());
      size_t n = ts_std_list_len (&list);
      if (n > max_args) fail (TS_ERR_LIMIT, "shell: too many positional arguments");
      std::vector<std::string> args;
      for (size_t i = 0; i < n; i++) args.emplace_back (text (*ts_std_list_get (&list, i)));
      shell.set_positional (std::move (args));
    }
  return strings (ret, shell.positional (), error);
}

TS_Status
s_timeout (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 2);
  auto &timeout = state (argv[0]).shell->options ().timeout;
  if (argc == 2)
    {
      int64_t ms = integer (argv[1], 0, INT32_MAX);
      if (ms) timeout = lsh::Timeout {std::chrono::milliseconds (ms)};
      else timeout.reset ();
    }
  ts_std_ret_int (ret, timeout ? timeout->duration.count () : 0);
  return TS_OK;
}

TS_Status
s_output_limit (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 2);
  auto &limit = state (argv[0]).output_limit;
  if (argc == 2) limit = static_cast<size_t> (integer (argv[1], 1, max_output));
  ts_std_ret_int (ret, static_cast<int64_t> (limit));
  return TS_OK;
}

TS_Status
s_pipefail (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 2);
  auto &policy = state (argv[0]).shell->options ().pipefail;
  if (argc == 2) policy = boolean (argv[1]) ? lsh::PipefailPolicy::any_failed : lsh::PipefailPolicy::last;
  ts_std_ret_bool (ret, policy == lsh::PipefailPolicy::any_failed);
  return TS_OK;
}

TS_Status
s_word (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  return wrap (ret, std::make_unique<lsh::Argument> (parse_word (text (argv[0]))),
               "shell word", &word_tag);
}

TS_Status
s_expand (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *error)
{
  arity (argc, 2, 2);
  auto &shell = *state (argv[0]).shell;
  auto arg = argv[1].type == TS_STRING ? parse_word (text (argv[1])) : word (argv[1]);
  auto expanded = shell.expander ()->expand_argv ({std::move (arg)}, shell.context ());
  if (!expanded) diagnostic (expanded.error ());
  return strings (ret, expanded.value (), error);
}

TS_Status
s_parse (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  return program_result (ret, parse (text (argv[0])));
}

TS_Status
s_command (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  lsh::ir::Command cmd;
  items (argv, argc, [&](const TS_Value &v) { cmd.argv.push_back (word (v)); });
  return program_result (ret, lsh::ir::command (std::move (cmd)));
}

TS_Status
s_pipeline (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  lsh::ir::Pipeline pipeline;
  items (argv, argc, [&](const TS_Value &v) {
    const auto &root = handle<Program> (v, &program_tag).root;
    const auto *cmd = std::get_if<lsh::ir::Command> (&root->value);
    if (!cmd) fail (TS_ERR_INVAL, "shell: pipeline stages must be simple commands");
    pipeline.commands.push_back (*cmd);
  });
  return program_result (ret, lsh::ir::node (std::move (pipeline)));
}

TS_Status
s_sequence (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 3);
  lsh::ir::Sequence seq;
  seq.left = handle<Program> (argv[0], &program_tag).root;
  seq.right = handle<Program> (argv[1], &program_tag).root;
  const std::string_view kind = argc == 3 ? text (argv[2]) : "sequence";
  if (kind == "and") seq.connective = lsh::Connective::and_if;
  else if (kind == "or") seq.connective = lsh::Connective::or_if;
  else if (kind != "sequence") fail (TS_ERR_INVAL, "shell: unknown connective");
  return program_result (ret, lsh::ir::node (std::move (seq)));
}

TS_Status
s_redirect (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 3, 4);
  lsh::Redirection r;
  const std::string_view stream = text (argv[1]);
  if (stream == "stdin") r.stream = lsh::RedirectStream::stdin_stream;
  else if (stream == "stdout") r.stream = lsh::RedirectStream::stdout_stream;
  else if (stream == "stderr") r.stream = lsh::RedirectStream::stderr_stream;
  else fail (TS_ERR_INVAL, "shell: unknown redirection stream");
  const std::string_view mode = argc == 4 ? text (argv[3]) : stream == "stdin" ? "read" : "truncate";
  if (mode == "read") r.mode = lsh::RedirectMode::read;
  else if (mode == "truncate") r.mode = lsh::RedirectMode::truncate;
  else if (mode == "append") r.mode = lsh::RedirectMode::append;
  else fail (TS_ERR_INVAL, "shell: unknown redirection mode");
  r.target.kind = lsh::StdioTargetKind::file;
  r.target.file = text (argv[2]);
  auto root = handle<Program> (argv[0], &program_tag).root;
  auto copy = std::make_shared<lsh::ir::Node> (*root);
  if (auto *cmd = std::get_if<lsh::ir::Command> (&copy->value))
    cmd->redirections.push_back (std::move (r));
  else if (auto *pipeline = std::get_if<lsh::ir::Pipeline> (&copy->value))
    (stream == "stdin" ? pipeline->commands.front () : pipeline->commands.back ()).redirections.push_back (std::move (r));
  else fail (TS_ERR_INVAL, "shell: redirect requires a command or pipeline");
  return program_result (ret, std::move (copy));
}

template <bool capture>
TS_Status
execute (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 2, 3);
  State &s = state (argv[0]);
  auto root = program (argv[1]);
  auto input = argc == 3 ? std::make_shared<lsh::MemoryReader> (text (argv[2])) : nullptr;
  auto out = capture ? std::make_shared<Capture> (s.output_limit) : nullptr;
  auto err = capture ? std::make_shared<Capture> (s.output_limit) : nullptr;
  struct Streams
  {
    Executor &executor;
    ~Streams () { executor.input.reset (); executor.output.reset (); executor.errors.reset (); }
  } streams { *s.executor };
  s.executor->input = std::move (input);
  s.executor->output = out;
  s.executor->errors = err;
  s.shell->clear_signal ();
  s.shell->cancel_exit ();
  s.output.clear ();
  s.errors.clear ();
  s.report = {};
  errno = 0;
  auto result = s.shell->run (root);
  int saved_errno = errno;
  if (out) s.output = std::move (out->data);
  if (err) s.errors = std::move (err->data);
  if (out && (out->error != TS_OK || err->error != TS_OK))
    {
      TS_Status status = out->error != TS_OK ? out->error : err->error;
      s.report.status.code = 1;
      fail (status, status == TS_ERR_UNSUPPORTED ? "shell: NUL bytes in captured output"
                                               : "shell: capture limit/allocation failure");
    }
  if (!result)
    {
      s.report.status.code = 1;
      s.report.diagnostics.push_back (result.error ());
      diagnostic (result.error (), saved_errno);
    }
  s.report = std::move (result).value ();
  if constexpr (capture) check (ts_value_make_string (ret, s.output.c_str ()));
  else ts_std_ret_int (ret, s.report.status.code);
  return TS_OK;
}

TS_Status
s_status (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  ts_std_ret_int (ret, state (argv[0]).report.status.code);
  return TS_OK;
}

template <bool stderr_stream>
TS_Status
captured (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *)
{
  arity (argc, 1, 1);
  const State &s = state (argv[0]);
  check (ts_value_make_string (ret, (stderr_stream ? s.errors : s.output).c_str ()));
  return TS_OK;
}

using Dom = std::unique_ptr<ts_dom_t, decltype (&ts_dom_free)>;

Dom
dom (ts_dom_t *p)
{
  if (!p) check (TS_ERR_NOMEM);
  return Dom (p, ts_dom_free);
}

void
field (ts_dom_t *object, const char *key, Dom value)
{
  check (ts_dom_obj_set (object, key, value.release ()));
}

Dom
exit_status (const lsh::ExitStatus &status)
{
  Dom result = dom (ts_dom_new_obj ());
  field (result.get (), "code", dom (ts_dom_new_int (status.code)));
  field (result.get (), "ok", dom (ts_dom_new_bool (status.success ())));
  field (result.get (), "signaled", dom (ts_dom_new_bool (status.signaled)));
  field (result.get (), "signal", dom (status.signal ? ts_dom_new_int (*status.signal) : ts_dom_new_null ()));
  field (result.get (), "timed_out", dom (ts_dom_new_bool (status.timed_out)));
  field (result.get (), "cancelled", dom (ts_dom_new_bool (status.canceled)));
  return result;
}

TS_Status
s_result (const TS_Value *argv, size_t argc, TS_Value *ret, TS_Error *error)
{
  arity (argc, 1, 1);
  const State &s = state (argv[0]);
  Dom result = exit_status (s.report.status);
  field (result.get (), "pid", dom (s.report.pid ? ts_dom_new_int (*s.report.pid) : ts_dom_new_null ()));
  field (result.get (), "stdout", dom (ts_dom_new_str (s.output.c_str ())));
  field (result.get (), "stderr", dom (ts_dom_new_str (s.errors.c_str ())));
  Dom pipeline = dom (ts_dom_new_arr ());
  for (const auto &status : s.report.pipeline_statuses)
    check (ts_dom_arr_push (pipeline.get (), exit_status (status).release ()));
  field (result.get (), "pipeline", std::move (pipeline));
  Dom diagnostics = dom (ts_dom_new_arr ());
  for (const auto &d : s.report.diagnostics)
    {
      Dom entry = dom (ts_dom_new_obj ());
      field (entry.get (), "code", dom (ts_dom_new_int (static_cast<int> (d.code))));
      field (entry.get (), "message", dom (ts_dom_new_str (d.message.c_str ())));
      field (entry.get (), "path", dom (ts_dom_new_str (d.path.c_str ())));
      check (ts_dom_arr_push (diagnostics.get (), entry.release ()));
    }
  field (result.get (), "diagnostics", std::move (diagnostics));
  return ts_dom_wrap (ret, result.release (), error);
}

/* No C++ exception may cross the native extension's C ABI. */
template <TS_Status (*F) (const TS_Value *, size_t, TS_Value *, TS_Error *)>
TS_Status
native (TS_VM *, void *, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  try { return F (argv, argc, ret, error); }
  catch (const Fault &f)
    { ts_error_set (error, f.status, f.system_errno, f.offset, "%s", f.message.c_str ()); return f.status; }
  catch (const std::bad_alloc &)
    { return ts_std_oom (error); }
  catch (const std::system_error &e)
    { ts_error_set (error, TS_ERR_SYSTEM, e.code ().value (), 0, "shell: %s", e.what ()); return TS_ERR_SYSTEM; }
  catch (const std::exception &e)
    { ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "shell: %s", e.what ()); return TS_ERR_SYSTEM; }
  catch (...)
    { ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "shell: native exception"); return TS_ERR_SYSTEM; }
}

const TS_FuncDef shell_funcs[] = {
  { "new", native<s_new>, nullptr },
  { "close", native<s_close>, nullptr },
  { "get", native<s_get>, nullptr },
  { "set", native<s_set>, nullptr },
  { "unset", native<s_unset>, nullptr },
  { "export", native<s_export>, nullptr },
  { "cwd", native<s_cwd>, nullptr },
  { "cd", native<s_cd>, nullptr },
  { "args", native<s_args>, nullptr },
  { "timeout", native<s_timeout>, nullptr },
  { "output_limit", native<s_output_limit>, nullptr },
  { "pipefail", native<s_pipefail>, nullptr },
  { "word", native<s_word>, nullptr },
  { "expand", native<s_expand>, nullptr },
  { "parse", native<s_parse>, nullptr },
  { "command", native<s_command>, nullptr },
  { "pipeline", native<s_pipeline>, nullptr },
  { "sequence", native<s_sequence>, nullptr },
  { "redirect", native<s_redirect>, nullptr },
  { "run", native<execute<false>>, nullptr },
  { "capture", native<execute<true>>, nullptr },
  { "status", native<s_status>, nullptr },
  { "stdout", native<captured<false>>, nullptr },
  { "stderr", native<captured<true>>, nullptr },
  { "result", native<s_result>, nullptr },
  { nullptr, nullptr, nullptr }
};

} // namespace

extern "C" const TS_Module ts_std_shell_module = { "std.shell", shell_funcs };
