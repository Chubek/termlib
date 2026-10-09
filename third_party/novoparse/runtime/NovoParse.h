#ifndef NOVOPARSE_H
#define NOVOPARSE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NP_NONTERMINAL, NP_LITERAL, NP_PATTERN } np_symbol_kind;
typedef enum { NP_ASSOC_NONE, NP_ASSOC_LEFT, NP_ASSOC_RIGHT, NP_ASSOC_NONASSOC } np_associativity;
typedef enum { NP_REJECT, NP_FIRST } np_disambiguation;
typedef enum { NP_SKIP_NONE, NP_SKIP_LITERAL, NP_SKIP_PATTERN } np_skip_kind;
typedef enum {
    NP_SUCCESS, NP_ERROR_SYNTAX, NP_ERROR_MEMORY, NP_ERROR_GRAMMAR,
    NP_ERROR_AMBIGUOUS, NP_ERROR_SEMANTIC
} np_status;

typedef struct {
    np_symbol_kind kind;
    const char *name;
    const char *value;
    int precedence;
    np_associativity associativity;
} np_symbol_spec;

typedef struct {
    int head;
    const int *body;
    size_t body_length;
    const char *label;
    int precedence;
    np_associativity associativity;
} np_production_spec;

typedef struct {
    const char *name;
    const np_symbol_spec *symbols;
    size_t symbol_count;
    const np_production_spec *productions;
    size_t production_count;
    int start;
    np_skip_kind skip_kind;
    const char *skip;
    np_disambiguation disambiguation;
    const char *rewrites; /* A GRL program, or NULL/empty for none. */
} np_grammar_spec;

typedef struct np_parser np_parser;
typedef struct np_node np_node;

/* Descriptors are copied during creation. Error buffers may be NULL. */
np_parser *np_parser_create(const np_grammar_spec *spec, char *error, size_t error_size);
void np_parser_destroy(np_parser *parser);
/* Input is copied. Results are borrowed until the next parse or destruction.
   A parser is reusable; distinct instances may be used in distinct threads. */
np_status np_parser_parse(np_parser *parser, const char *input, size_t length);
const np_node *np_parser_root(const np_parser *parser);
const char *np_parser_json(np_parser *parser);
const char *np_parser_error(const np_parser *parser);
size_t np_parser_position(const np_parser *parser);
size_t np_parser_line(const np_parser *parser);
size_t np_parser_column(const np_parser *parser);
int np_parser_ambiguous(const np_parser *parser);
size_t np_parser_conflicts(const np_parser *parser);

const char *np_node_name(const np_node *node);
const char *np_node_text(const np_node *node); /* NULL for nonterminals. */
size_t np_node_text_length(const np_node *node);
size_t np_node_start(const np_node *node);
size_t np_node_end(const np_node *node);
size_t np_node_child_count(const np_node *node);
const np_node *np_node_child(const np_node *node, size_t index);

#ifdef __cplusplus
}
#endif
#endif
