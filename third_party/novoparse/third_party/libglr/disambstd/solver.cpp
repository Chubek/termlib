#include <glr/disambstd.h>
#include "SatieCDCL.hpp"
#include "SatieIDL.hpp"
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>

namespace {
/* Use a conservative numeric domain: Satie's Bellman-Ford uses int64_t
   potentials, so reserve headroom for every relaxation. */
void add_layout (satie::idl::IDLSolver &solver,
                 const glr_disambstd_layout_t &layout, size_t combined_constraints = 0)
{
  if (layout.variable_count > 100000 || layout.constraint_count > 100000 ||
      (layout.constraint_count && !layout.constraints))
    throw std::invalid_argument ("invalid layout");
  for (size_t i = 0; i < layout.variable_count; ++i)
    solver.add_var ("v" + std::to_string (i));
  const int64_t limit = INT64_MAX / 4
      / static_cast<int64_t> (solver.variable_count () + 1)
      / static_cast<int64_t> (std::max (layout.constraint_count, combined_constraints) + 1);
  for (size_t i = 0; i < layout.constraint_count; ++i)
    {
      const auto &c = layout.constraints[i];
      if (c.x >= layout.variable_count || c.y >= layout.variable_count
          || c.upper < -limit || c.upper > limit)
        throw std::invalid_argument ("invalid difference constraint");
      solver.add_le (static_cast<int> (c.x), static_cast<int> (c.y), c.upper);
    }
}

struct Derivation {
  std::vector<int> tokens, productions;
  glr_disambstd_derivation_t view () const
  { return {tokens.data (), tokens.size (), productions.data (), productions.size ()}; }
};
struct Symbol { glr_symbol_t *symbol; size_t depth; };
struct Enumeration {
  const glr_grammar_t &grammar;
  const glr_disambstd_options_t &options;
  std::map<std::vector<int>, std::vector<Derivation>> groups;
  size_t visited = 0;
  bool limited = false;

  void expand (const std::vector<Symbol> &form, const std::vector<int> &trace)
  {
    struct Pending { std::vector<Symbol> form; std::vector<int> trace; };
    std::vector<Pending> pending {{form, trace}};
    while (!pending.empty () && !limited)
      {
        auto item = std::move (pending.back ()); pending.pop_back ();
        const auto &form = item.form;
        const auto &trace = item.trace;
        if (++visited > options.max_derivations) { limited = true; break; }
    size_t terminal_count = 0, next = form.size ();
    for (size_t i = 0; i < form.size (); ++i)
      if (form[i].symbol->type == GLR_SYMBOL_TERMINAL) ++terminal_count;
      else if (next == form.size ()) next = i;
    if (terminal_count > options.max_tokens) continue;
    if (next == form.size ())
      {
        Derivation d;
        for (auto s : form) d.tokens.push_back (s.symbol->id);
        d.productions = trace;
        groups[d.tokens].push_back (std::move (d));
        continue;
      }
    if (form[next].depth >= options.max_depth) continue;
    for (size_t p = 0; p < grammar.production_count && !limited; ++p)
      {
        const auto *production = grammar.productions[p];
        if (production->head != form[next].symbol) continue;
        std::vector<Symbol> child (form.begin (), form.begin () + next);
        for (size_t j = 0; j < production->body_length; ++j)
          child.push_back ({production->body[j], form[next].depth + 1});
        child.insert (child.end (), form.begin () + next + 1, form.end ());
        auto path = trace;
        path.push_back (production->id);
        if (pending.size () >= options.max_derivations)
          { limited = true; break; }
        pending.push_back ({std::move (child), std::move (path)});
      }
      }
  }
};

struct Layout {
  size_t variables = 0;
  std::vector<glr_disambstd_constraint_t> constraints;
  glr_disambstd_layout_t view () const
  { return {variables, constraints.data (), constraints.size ()}; }
};

bool compatible (const Layout &a, const Layout &b, std::vector<int64_t> &model)
{
  satie::idl::IDLSolver solver;
  /* Shared variable indices describe the same sentence's layout. */
  auto av = a.view (), bv = b.view ();
  const size_t n = std::max (av.variable_count, bv.variable_count);
  for (size_t i = 0; i < n; ++i) solver.add_var ("v" + std::to_string (i));
  add_layout (solver, av, av.constraint_count + bv.constraint_count);
  add_layout (solver, bv, av.constraint_count + bv.constraint_count);
  if (!solver.check ().satisfiable ()) return false;
  model.clear ();
  if (n)
    for (size_t i = 0; i < n; ++i)
      model.push_back (solver.value (static_cast<int> (i)) - solver.value (0));
  return true;
}

int *copy (const std::vector<int> &v)
{
  if (v.empty ()) return nullptr;
  auto *p = static_cast<int *> (std::malloc (v.size () * sizeof (int)));
  if (!p) throw std::bad_alloc ();
  std::memcpy (p, v.data (), v.size () * sizeof (int));
  return p;
}
void witness (glr_disambstd_report_t &r, const Derivation &a, const Derivation &b)
{
  r.tokens = copy (a.tokens); r.token_count = a.tokens.size ();
  r.productions[0] = copy (a.productions); r.production_count[0] = a.productions.size ();
  r.productions[1] = copy (b.productions); r.production_count[1] = b.productions.size ();
}
}

extern "C" int glr_std_layout_check (const glr_disambstd_layout_t *layout)
{
  try {
    if (!layout) return -1;
    satie::idl::IDLSolver solver;
    add_layout (solver, *layout);
    return solver.check ().satisfiable () ? 1 : 0;
  } catch (...) { return -1; }
}

extern "C" void glr_disambstd_report_clear (glr_disambstd_report_t *r)
{
  if (!r) return;
  std::free (r->tokens);
  std::free (r->productions[0]); std::free (r->productions[1]);
  std::free (r->layout_values);
  std::memset (r, 0, sizeof (*r));
}

extern "C" glr_disambstd_check_result_t glr_disambstd_check (
    glr_disambstd_kind_t kind, const glr_grammar_t *grammar,
    const glr_disambstd_options_t *options, glr_disambstd_report_t *report)
{
  if (!report) return GLR_DISAMBSTD_CHECK_ERROR;
  glr_disambstd_report_clear (report);
  report->result = GLR_DISAMBSTD_CHECK_ERROR;
  try {
    glr_disambstd_options_t defaults;
    glr_disambstd_options_init (&defaults);
    const auto &o = options ? *options : defaults;
    if (!grammar || !glr_grammar_validate (grammar, nullptr, 0)
        || (kind != GLR_DISAMBSTD_BOUNDED_SAT && kind != GLR_DISAMBSTD_BOUNDED_SMT
            && kind != GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED)
        || !o.max_depth || o.max_depth > 256 || !o.max_derivations
        || o.max_derivations > 1000000 || !o.max_iterations
        || (kind == GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED && (!o.refine || !o.derivation_filter)))
      return report->result;
    Enumeration enumeration {*grammar, o, {}, 0, false};
    enumeration.expand ({{grammar->start_symbol, 0}}, {});
    /* Check shorter strings first, independent of production order. */
    std::vector<const std::vector<Derivation> *> groups;
    for (const auto &entry : enumeration.groups)
      if (entry.second.size () > 1) groups.push_back (&entry.second);
    std::stable_sort (groups.begin (), groups.end (), [] (auto a, auto b) {
      return a->front ().tokens.size () < b->front ().tokens.size ();
    });
    for (size_t iteration = 0; iteration < o.max_iterations; ++iteration)
      {
        report->iterations = iteration + 1;
        bool refined = false;
        for (auto group : groups)
          {
            std::vector<const Derivation *> active;
            std::vector<Layout> layouts;
            for (const auto &d : *group)
              {
                auto view = d.view ();
                glr_disambstd_layout_t layout {};
                if (o.derivation_filter && !o.derivation_filter (&view, &layout, o.user_data)) continue;
                Layout saved;
                if (kind != GLR_DISAMBSTD_BOUNDED_SAT)
                  {
                    if (glr_std_layout_check (&layout) < 0)
                      throw std::invalid_argument ("invalid layout");
                    saved.variables = layout.variable_count;
                    if (layout.constraint_count)
                      saved.constraints.assign (layout.constraints, layout.constraints + layout.constraint_count);
                  }
                active.push_back (&d); layouts.push_back (std::move (saved));
              }
            if (active.size () < 2) continue;
            /* Two one-hot derivation selectors. Equality of terminal yields
               is guaranteed by the group; diagonal pairs are forbidden.
               SAT chooses distinct derivations, IDL refines infeasible pairs. */
            const int n = static_cast<int> (active.size ());
            if (n > 2048) { report->result = GLR_DISAMBSTD_CHECK_LIMIT; return report->result; }
            satie::CNF cnf;
            satie::Clause left, right;
            for (int i = 1; i <= n; ++i)
              {
                left.push_back (i); right.push_back (n + i);
                cnf.add_clause ({-i, -(n + i)});
                for (int j = 1; j < i; ++j)
                  {
                    cnf.add_clause ({-i, -j});
                    cnf.add_clause ({-(n + i), -(n + j)});
                  }
              }
            cnf.add_clause (left); cnf.add_clause (right);
            for (;;)
              {
                satie::CDCLSolver solver (cnf);
                auto result = solver.solve ();
                if (result.unsatisfiable ()) break;
                if (!result.satisfiable ())
                  { report->result = GLR_DISAMBSTD_CHECK_LIMIT; return report->result; }
                size_t a = 0, b = 0;
                for (int i = 1; i <= n; ++i)
                  {
                    if (result.assignment.get_var (i) == satie::Value::TRUE) a = i - 1;
                    if (result.assignment.get_var (n + i) == satie::Value::TRUE) b = i - 1;
                  }
                std::vector<int64_t> model;
                if (kind != GLR_DISAMBSTD_BOUNDED_SAT && !compatible (layouts[a], layouts[b], model))
                  {
                    cnf.add_clause ({-static_cast<int> (a + 1), -static_cast<int> (n + b + 1)});
                    cnf.add_clause ({-static_cast<int> (b + 1), -static_cast<int> (n + a + 1)});
                    continue;
                  }
                auto av = active[a]->view (), bv = active[b]->view ();
                if (kind == GLR_DISAMBSTD_COUNTER_EXAMPLE_GUIDED && o.refine (&av, &bv, o.user_data))
                  { refined = true; break; }
                witness (*report, *active[a], *active[b]);
                if (!model.empty ())
                  {
                    report->layout_values = static_cast<int64_t *> (
                        std::malloc (model.size () * sizeof (int64_t)));
                    if (!report->layout_values) throw std::bad_alloc ();
                    std::memcpy (report->layout_values, model.data (), model.size () * sizeof (int64_t));
                    report->layout_variable_count = model.size ();
                  }
                report->result = GLR_DISAMBSTD_CHECK_AMBIGUOUS;
                return report->result;
              }
            if (refined) break;
          }
        if (!refined)
          {
            report->result = enumeration.limited ? GLR_DISAMBSTD_CHECK_LIMIT : GLR_DISAMBSTD_CHECK_CLEAR;
            return report->result;
          }
      }
    report->result = GLR_DISAMBSTD_CHECK_LIMIT;
  } catch (...) {
    glr_disambstd_report_clear (report);
    report->result = GLR_DISAMBSTD_CHECK_ERROR;
  }
  return report->result;
}
