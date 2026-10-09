#include "NovoParse.h"
#include <glr/parser.h>
#include <glr/scannerless.h>
#include <glr/rewrite.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MODE_NODE, MODE_TOKEN, MODE_SKIP, MODE_ROOT };
typedef struct {
    char *label;
    int precedence;
    np_associativity associativity;
    int mode;
} production_data;

struct np_node {
    const char *name;
    char *text;
    size_t text_length, start, end;
    struct np_node **children;
    size_t child_count;
    int transparent;
    struct np_node *next;
};

struct np_parser {
    glr_grammar_t *grammar;
    glr_parser_t *engine;
    production_data **metadata;
    size_t metadata_count, metadata_capacity;
    np_disambiguation disambiguation;
    np_node *nodes, *root;
    char *input, *json;
    size_t length, position, line, column, conflicts;
    int ambiguous;
    char error[512];
};

static char *copy_text(const char *text, size_t length)
{
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = '\0'; }
    return copy;
}

static void clear_result(np_parser *parser)
{
    while (parser->nodes) {
        np_node *node = parser->nodes;
        parser->nodes = node->next;
        free(node->text); free(node->children); free(node);
    }
    free(parser->input); parser->input = NULL;
    free(parser->json); parser->json = NULL;
    parser->root = NULL;
    parser->length = parser->position = 0;
    parser->line = parser->column = 1;
    parser->ambiguous = 0;
    parser->error[0] = '\0';
}

void np_parser_destroy(np_parser *parser)
{
    if (!parser) return;
    clear_result(parser);
    glr_parser_destroy(parser->engine);
    glr_grammar_destroy(parser->grammar);
    for (size_t i = 0; i < parser->metadata_count; ++i) {
        free(parser->metadata[i]->label); free(parser->metadata[i]);
    }
    free(parser->metadata);
    free(parser);
}

static np_node *new_node(np_parser *parser, const char *name, size_t start, size_t end)
{
    np_node *node = calloc(1, sizeof *node);
    if (!node) return NULL;
    node->name = name; node->start = start; node->end = end;
    node->next = parser->nodes; parser->nodes = node;
    return node;
}

static np_node *terminal_node(np_parser *parser, const glr_forest_node_t *terminal)
{
    if (!terminal || !parser || !parser->input) return NULL;
    glr_symbol_t *symbol = glr_grammar_get_symbol(parser->grammar, terminal->symbol_id);
    if (!symbol || !symbol->name) return NULL;
    if (terminal->position > terminal->end_position) return NULL;
    if (terminal->end_position > parser->length) return NULL;
    np_node *node = new_node(parser, symbol->name, terminal->position, terminal->end_position);
    if (!node) return NULL;
    node->text_length = node->end - node->start;
    node->text = copy_text(parser->input + node->start, node->text_length);
    return node->text ? node : NULL;
}

static int build_node(const glr_semantic_context_t *context, void **value, void *data)
{
    if (!context || !value) return -1;
    np_parser *parser = context->user_data;
    production_data *metadata = data;
    const glr_select_context_t *selection = &context->selection;
    if (!parser || !metadata || !selection) return -1;
    *value = NULL;
    if (metadata->mode == MODE_SKIP) return 0;
    if (metadata->mode == MODE_ROOT) {
        if (!selection->values || selection->child_count < 2) return -1;
        *value = selection->values[1];
        return 0;
    }
    if (metadata->mode == MODE_TOKEN) {
        if (!selection->children || !selection->child_count) return -1;
        *value = terminal_node(parser, selection->children[0]);
        return *value ? 0 : -1;
    }
    if (!selection->production || !selection->production->head || !selection->production->head->name) return -1;
    const char *name = metadata->label ? metadata->label : selection->production->head->name;
    if (!context->constructor) return -1;
    np_node *node = new_node(parser, name, context->constructor->position, context->constructor->end_position);
    if (!node) return -1;
    node->transparent = strncmp(selection->production->head->name, "__np_ebnf_", 10) == 0;
    if (selection->child_count && (!selection->children || !selection->values)) return -1;
    np_node **children = calloc(selection->child_count ? selection->child_count : 1, sizeof *children);
    if (!children) return -1;
    size_t count = 0;
    for (size_t i = 0; i < selection->child_count; ++i) {
        if (!selection->children[i]) { free(children); return -1; }
        children[i] = selection->children[i]->type == GLR_NODE_TERMINAL
            ? terminal_node(parser, selection->children[i]) : selection->values[i];
        if (selection->children[i]->type == GLR_NODE_TERMINAL && !children[i]) { free(children); return -1; }
        if (children[i]) {
            size_t extra = children[i]->transparent ? children[i]->child_count : 1;
            if (extra > SIZE_MAX - count) { free(children); return -1; }
            count += extra;
        }
    }
    node->children = calloc(count ? count : 1, sizeof *node->children);
    if (!node->children) { free(children); return -1; }
    for (size_t i = 0; i < selection->child_count; ++i) {
        np_node *child = children[i];
        if (!child) continue;
        if (child->transparent) {
            if (child->child_count && !child->children) { free(children); return -1; }
            for (size_t j = 0; j < child->child_count; ++j) node->children[node->child_count++] = child->children[j];
        } else node->children[node->child_count++] = child;
    }
    free(children);
    if (node->child_count) {
        node->start = node->children[0]->start;
        node->end = node->children[node->child_count - 1]->end;
    }
    *value = node;
    return 0;
}

static size_t choose_first(const glr_forest_node_t *symbol, void *data)
{
    (void)data;
    if (!symbol || !symbol->child_count || !symbol->children) return 0;
    size_t winner = 0;
    for (size_t i = 1; i < symbol->child_count; ++i) {
        const glr_forest_node_t *a = symbol->children[i], *b = symbol->children[winner];
        if (!a || !b) continue;
        if (a->symbol_id < b->symbol_id) winner = i;
        else if (a->symbol_id == b->symbol_id) {
            if (!a->children || !b->children) continue;
            for (size_t j = 0; j < a->child_count && j < b->child_count; ++j) {
                if (!a->children[j] || !b->children[j]) break;
                if (a->children[j]->end_position == b->children[j]->end_position) continue;
                if (a->children[j]->end_position > b->children[j]->end_position) winner = i;
                break;
            }
        }
    }
    return winner;
}

static int add_initial_grammar(np_parser *parser, const np_grammar_spec *spec)
{
    glr_grammar_t *grammar = parser->grammar;
    for (size_t i = 0; i < spec->symbol_count; ++i) {
        const np_symbol_spec *symbol = &spec->symbols[i];
        if (!symbol->name || !*symbol->name || symbol->kind < NP_NONTERMINAL || symbol->kind > NP_PATTERN
            || symbol->precedence < 0 || symbol->associativity < NP_ASSOC_NONE || symbol->associativity > NP_ASSOC_NONASSOC
            || ((symbol->precedence == 0) != (symbol->associativity == NP_ASSOC_NONE))) {
            snprintf(parser->error, sizeof parser->error, "invalid symbol descriptor at index %zu", i); return -1;
        }
        if (glr_grammar_find_symbol_any(grammar, symbol->name) >= 0) {
            snprintf(parser->error, sizeof parser->error, "duplicate symbol: %s", symbol->name); return -1;
        }
        int id = glr_grammar_add_symbol(grammar, symbol->kind == NP_NONTERMINAL ? GLR_SYMBOL_NONTERMINAL : GLR_SYMBOL_TERMINAL, symbol->name);
        if (id < 0) return -1;
        if (symbol->kind != NP_NONTERMINAL) {
            if (!symbol->value || !*symbol->value) {
                snprintf(parser->error, sizeof parser->error, "empty terminal: %s", symbol->name); return -1;
            }
            int status = symbol->kind == NP_PATTERN
                ? glr_scannerless_set_pattern(grammar, id, symbol->value, parser->error, sizeof parser->error)
                : glr_scannerless_set_literal(grammar, id, symbol->value, strlen(symbol->value));
            if (status != 0) return -1;
        }
    }
    for (size_t i = 0; i < spec->production_count; ++i) {
        const np_production_spec *production = &spec->productions[i];
        if (production->body_length > SIZE_MAX / sizeof(glr_symbol_t *)
            || (production->body_length && !production->body)
            || production->body_length > INT_MAX
            || production->head < 0 || (size_t)production->head >= spec->symbol_count
            || production->precedence < 0 || production->associativity < NP_ASSOC_NONE || production->associativity > NP_ASSOC_NONASSOC
            || ((production->precedence == 0) != (production->associativity == NP_ASSOC_NONE))) {
            snprintf(parser->error, sizeof parser->error, "invalid production descriptor at index %zu", i); return -1;
        }
        for (size_t j = 0; j < production->body_length; ++j) {
            if (production->body[j] < 0 || (size_t)production->body[j] >= spec->symbol_count) {
                snprintf(parser->error, sizeof parser->error, "invalid symbol ID in production %zu", i); return -1;
            }
        }
        glr_symbol_t **body = calloc(production->body_length ? production->body_length : 1, sizeof *body);
        if (!body) return -1;
        for (size_t j = 0; j < production->body_length; ++j) body[j] = glr_grammar_get_symbol(grammar, production->body[j]);
        int id = glr_grammar_add_production(grammar, production->head, body, production->body_length);
        free(body);
        if (id < 0) { snprintf(parser->error, sizeof parser->error, "invalid production at index %zu", i); return -1; }
        if (production->precedence || (production->label && *production->label)) {
            char annotation[32];
            snprintf(annotation, sizeof annotation, "%zu", i);
            grammar->productions[id]->annotation = copy_text(annotation, strlen(annotation));
            if (!grammar->productions[id]->annotation) return -1;
        }
    }
    return glr_grammar_set_start_symbol(grammar, spec->start);
}

static int add_metadata(np_parser *parser, const char *label, int precedence, np_associativity associativity, int mode)
{
    if (parser->metadata_count >= parser->metadata_capacity) return -1;
    production_data *data = calloc(1, sizeof *data);
    if (!data) return -1;
    if (label && *label) {
        data->label = copy_text(label, strlen(label));
        if (!data->label) { free(data); return -1; }
    }
    data->precedence = precedence;
    data->associativity = associativity;
    data->mode = mode;
    parser->metadata[parser->metadata_count++] = data;
    return 0;
}

static int prepare_metadata(np_parser *parser, const np_grammar_spec *spec)
{
    glr_grammar_t *grammar = parser->grammar;
    if (grammar->production_count > SIZE_MAX - 2 * grammar->symbol_count - 8) return -1;
    size_t capacity = grammar->production_count + 2 * grammar->symbol_count + 8;
    parser->metadata = calloc(capacity, sizeof *parser->metadata);
    if (!parser->metadata) return -1;
    parser->metadata_capacity = capacity;
    for (size_t i = 0; i < grammar->production_count; ++i) {
        glr_production_t *production = grammar->productions[i];
        const np_production_spec *original = NULL;
        if (production->annotation) {
            // Annotations are "%zu" indices written by add_initial_grammar;
            // rewrite passes preserve them. Validate strictly so a hostile
            // descriptor cannot alias the wrong precedence/label.
            char *end = NULL;
            unsigned long long index = strtoull(production->annotation, &end, 10);
            if (end && !*end && index < spec->production_count) original = &spec->productions[(size_t)index];
        }
        if (add_metadata(parser, original ? original->label : NULL,
                         original ? original->precedence : 0,
                         original ? original->associativity : NP_ASSOC_NONE, MODE_NODE) != 0) return -1;
    }
    return 0;
}

static int fresh_symbol(glr_grammar_t *grammar, glr_symbol_type_t type, const char *base)
{
    char name[96];
    for (size_t i = 0; i < 1000000; ++i) {
        snprintf(name, sizeof name, "__np_runtime_%s_%zu", base, i);
        if (glr_grammar_find_symbol_any(grammar, name) < 0) return glr_grammar_add_symbol(grammar, type, name);
    }
    return -1;
}

static int lower_skip(np_parser *parser, const np_grammar_spec *spec)
{
    if (spec->skip_kind == NP_SKIP_NONE) return 0;
    glr_grammar_t *grammar = parser->grammar;
    size_t symbol_count = grammar->symbol_count, production_count = grammar->production_count;
    int skip_token = fresh_symbol(grammar, GLR_SYMBOL_TERMINAL, "trivia");
    int skip = fresh_symbol(grammar, GLR_SYMBOL_NONTERMINAL, "skip");
    if (skip_token < 0 || skip < 0) return -1;
    const char *expression = spec->skip;
    char *pattern = NULL;
    if (spec->skip_kind == NP_SKIP_LITERAL) {
        size_t length = strlen(spec->skip);
        if (length > (SIZE_MAX - 4) / 2) return -1;
        pattern = malloc(length * 2 + 4);
        if (!pattern) return -1;
        size_t offset = 0;
        pattern[offset++] = '(';
        for (size_t i = 0; i < length; ++i) {
            if (strchr("\\.^$[]()|*+?{}", spec->skip[i])) pattern[offset++] = '\\';
            pattern[offset++] = spec->skip[i];
        }
        pattern[offset++] = ')'; pattern[offset++] = '+'; pattern[offset] = '\0';
        expression = pattern;
    }
    int status = glr_scannerless_set_pattern(grammar, skip_token, expression, parser->error, sizeof parser->error);
    free(pattern);
    if (status != 0) return -1;
    if (skip_token < 0 || skip < 0 || (size_t)skip_token >= grammar->symbol_count || (size_t)skip >= grammar->symbol_count
        || !grammar->symbols[skip_token] || !grammar->symbols[skip]) return -1;
    glr_symbol_t *body[] = {grammar->symbols[skip_token], grammar->symbols[skip]};
    if (glr_grammar_add_production(grammar, skip, NULL, 0) < 0
        || add_metadata(parser, NULL, 0, NP_ASSOC_NONE, MODE_SKIP) != 0
        || glr_grammar_add_production(grammar, skip, body, 1) < 0
        || add_metadata(parser, NULL, 0, NP_ASSOC_NONE, MODE_SKIP) != 0) return -1;
    int *wrappers = calloc(symbol_count, sizeof *wrappers);
    if (!wrappers) return -1;
    for (size_t i = 0; i < symbol_count; ++i) wrappers[i] = -1;
    for (size_t i = 0; i < symbol_count; ++i) {
        if (i >= grammar->symbol_count || !grammar->symbols[i]) { free(wrappers); return -1; }
        if (grammar->symbols[i]->type != GLR_SYMBOL_TERMINAL) continue;
        int wrapper = fresh_symbol(grammar, GLR_SYMBOL_NONTERMINAL, "token");
        if (wrapper < 0 || (size_t)wrapper >= grammar->symbol_count) { free(wrappers); return -1; }
        body[0] = grammar->symbols[i]; body[1] = grammar->symbols[skip];
        if (wrapper < 0 || glr_grammar_add_production(grammar, wrapper, body, 2) < 0
            || add_metadata(parser, NULL, 0, NP_ASSOC_NONE, MODE_TOKEN) != 0) { free(wrappers); return -1; }
        wrappers[i] = wrapper;
    }
    for (size_t i = 0; i < production_count; ++i) {
        glr_production_t *production = grammar->productions[i];
        if (!production) { free(wrappers); return -1; }
        for (size_t j = 0; j < production->body_length; ++j) {
            if (!production->body[j]) { free(wrappers); return -1; }
            if (production->body[j]->type == GLR_SYMBOL_TERMINAL) {
                int id = production->body[j]->id;
                if (id < 0 || (size_t)id >= symbol_count || wrappers[id] < 0
                    || (size_t)wrappers[id] >= grammar->symbol_count || !grammar->symbols[wrappers[id]]) {
                    free(wrappers); return -1;
                }
                production->body[j] = grammar->symbols[wrappers[id]];
            }
        }
    }
    free(wrappers);
    int root = fresh_symbol(grammar, GLR_SYMBOL_NONTERMINAL, "root");
    if (root < 0 || !grammar->start_symbol) return -1;
    body[0] = grammar->symbols[skip]; body[1] = grammar->start_symbol;
    if (root < 0 || glr_grammar_add_production(grammar, root, body, 2) < 0
        || add_metadata(parser, NULL, 0, NP_ASSOC_NONE, MODE_ROOT) != 0) return -1;
    return glr_grammar_set_start_symbol(grammar, root);
}

static int resolve_precedence(np_parser *parser, glr_parse_table_t *table, const np_grammar_spec *spec)
{
    if (!table || !parser || !parser->grammar || !spec) return -1;
    for (size_t state = 0; state < table->state_count; ++state) {
        if (!table->states) return -1;
        for (size_t terminal = 0; terminal < parser->grammar->symbol_count; ++terminal) {
            if (!table->states[state].action_table) return -1;
            glr_action_set_t *cell = &table->states[state].action_table[terminal];
            if (cell->action_count < 2) continue;
            if (!cell->actions) return -1;
            int precedence = 0;
            if (!parser->grammar->symbols[terminal]) continue;
            const char *name = parser->grammar->symbols[terminal]->name;
            if (!name) continue;
            for (size_t i = 0; i < spec->symbol_count; ++i)
                if (strcmp(name, spec->symbols[i].name) == 0) { precedence = spec->symbols[i].precedence; break; }
            if (!precedence) continue;
            unsigned char *rejected = calloc(cell->action_count, 1);
            if (!rejected) return -1;
            for (size_t i = 0; i < cell->action_count; ++i) {
                if (cell->actions[i].type != GLR_ACTION_SHIFT) continue;
                for (size_t j = 0; j < cell->action_count; ++j) {
                    if (cell->actions[j].type != GLR_ACTION_REDUCE) continue;
                    int pid = cell->actions[j].reduce.production_id;
                    if (pid < 0 || (size_t)pid >= parser->metadata_count || !parser->metadata[pid]) continue;
                    production_data *rule = parser->metadata[pid];
                    if (!rule->precedence) continue;
                    if (rule->precedence > precedence) rejected[i] = 1;
                    else if (rule->precedence < precedence) rejected[j] = 1;
                    else if (rule->associativity == NP_ASSOC_LEFT) rejected[i] = 1;
                    else if (rule->associativity == NP_ASSOC_RIGHT) rejected[j] = 1;
                    else if (rule->associativity == NP_ASSOC_NONASSOC) rejected[i] = rejected[j] = 1;
                }
            }
            size_t count = 0;
            for (size_t i = 0; i < cell->action_count; ++i) if (!rejected[i]) cell->actions[count++] = cell->actions[i];
            cell->action_count = count;
            free(rejected);
        }
    }
    return 0;
}

np_parser *np_parser_create(const np_grammar_spec *spec, char *error, size_t error_size)
{
    if (error && error_size) error[0] = '\0';
    if (!spec || !spec->symbols || !spec->productions || !spec->symbol_count || !spec->production_count
        || spec->symbol_count > INT_MAX || spec->production_count > INT_MAX
        || spec->disambiguation < NP_REJECT || spec->disambiguation > NP_FIRST
        || spec->skip_kind < NP_SKIP_NONE || spec->skip_kind > NP_SKIP_PATTERN
        || (spec->skip_kind != NP_SKIP_NONE && (!spec->skip || !*spec->skip))) {
        if (error && error_size) snprintf(error, error_size, "invalid grammar descriptor");
        return NULL;
    }
    np_parser *parser = calloc(1, sizeof *parser);
    if (!parser) { if (error && error_size) snprintf(error, error_size, "out of memory"); return NULL; }
    parser->disambiguation = spec->disambiguation;
    parser->line = parser->column = 1;
    parser->grammar = glr_grammar_create();
    if (!parser->grammar || add_initial_grammar(parser, spec) != 0) goto fail;
    if (spec->rewrites && *spec->rewrites) {
        glr_rewrite_program_t *program = glr_rewrite_program_parse(spec->rewrites, strlen(spec->rewrites), parser->error, sizeof parser->error);
        if (!program) goto fail;
        glr_rewrite_report_t report = {0};
        glr_rewrite_status_t status = glr_rewrite_program_apply(parser->grammar, program, &report);
        glr_rewrite_program_destroy(program);
        if (status != GLR_REWRITE_STATUS_OK) { snprintf(parser->error, sizeof parser->error, "rewrite failed: %.450s", report.message); goto fail; }
    }
    if (prepare_metadata(parser, spec) != 0 || lower_skip(parser, spec) != 0) goto fail;
    for (size_t i = 0; i < parser->grammar->production_count; ++i) {
        if (i >= parser->metadata_count || !parser->metadata[i]) goto fail;
        if (glr_production_set_semantic_action(parser->grammar, (int)i, build_node, parser->metadata[i], NULL) != 0) goto fail;
    }
    glr_parse_table_t *table = glr_grammar_build_parse_table(parser->grammar, parser->error, sizeof parser->error);
    if (!table) goto fail;
    if (resolve_precedence(parser, table, spec) != 0) { glr_parse_table_destroy(table); goto fail; }
    parser->conflicts = glr_parse_table_conflict_count(table);
    glr_grammar_set_parse_table(parser->grammar, table, true);
    parser->engine = glr_parser_create(parser->grammar);
    if (!parser->engine) goto fail;
    glr_parser_set_user_data(parser->engine, parser);
    glr_parser_set_scannerless(parser->engine, true);
    if (spec->disambiguation == NP_FIRST) glr_parser_set_semantic_resolver(parser->engine, choose_first);
    return parser;
fail:
    if (error && error_size) snprintf(error, error_size, "%s", *parser->error ? parser->error : "invalid grammar or out of memory");
    np_parser_destroy(parser);
    return NULL;
}

np_status np_parser_parse(np_parser *parser, const char *input, size_t length)
{
    if (!parser || !parser->engine) return NP_ERROR_GRAMMAR;
    clear_result(parser);
    if (!input && length) { snprintf(parser->error, sizeof parser->error, "input is NULL"); return NP_ERROR_SYNTAX; }
    parser->input = copy_text(input ? input : "", length);
    if (!parser->input) { snprintf(parser->error, sizeof parser->error, "out of memory"); return NP_ERROR_MEMORY; }
    parser->length = length;
    glr_parse_result_t result = glr_parse(parser->engine, parser->input, length);
    parser->position = result.position <= length ? result.position : length;
    // Scan the internal copy: the caller's buffer may be mutated or freed
    // concurrently, and input may be NULL for empty parses.
    for (size_t i = 0; i < parser->position; ++i) {
        if (parser->input[i] == '\n') { ++parser->line; parser->column = 1; }
        else ++parser->column;
    }
    parser->ambiguous = (parser->engine->forest && parser->engine->forest->root)
        ? glr_forest_is_ambiguous(parser->engine->forest->root) : 0;
    if (result.error == GLR_PARSE_SUCCESS) { parser->root = result.semantic_value; return NP_SUCCESS; }
    np_status status;
    const char *message;
    if (result.error == GLR_PARSE_ERROR_SEMANTIC && result.semantic_error == GLR_SEMANTIC_ERROR_AMBIGUOUS) {
        status = NP_ERROR_AMBIGUOUS; message = "ambiguous input; declare precedence or use %disambiguate first";
    } else if (result.error == GLR_PARSE_ERROR_MEMORY || result.semantic_error == GLR_SEMANTIC_ERROR_MEMORY
               || result.semantic_error == GLR_SEMANTIC_ERROR_ACTION) {
        status = NP_ERROR_MEMORY; message = "out of memory";
    } else if (result.error == GLR_PARSE_ERROR_GRAMMAR) {
        status = NP_ERROR_GRAMMAR; message = "invalid grammar";
    } else if (result.error == GLR_PARSE_ERROR_SEMANTIC) {
        status = NP_ERROR_SEMANTIC; message = "cyclic or invalid semantic derivation";
    } else { status = NP_ERROR_SYNTAX; message = "syntax error"; }
    snprintf(parser->error, sizeof parser->error, "%s at byte %zu (line %zu, column %zu)", message, parser->position, parser->line, parser->column);
    return status;
}

const np_node *np_parser_root(const np_parser *p) { return p ? p->root : NULL; }
const char *np_parser_error(const np_parser *p) { return p ? p->error : "invalid parser"; }
size_t np_parser_position(const np_parser *p) { return p ? p->position : 0; }
size_t np_parser_line(const np_parser *p) { return p ? p->line : 1; }
size_t np_parser_column(const np_parser *p) { return p ? p->column : 1; }
int np_parser_ambiguous(const np_parser *p) { return p ? p->ambiguous : 0; }
size_t np_parser_conflicts(const np_parser *p) { return p ? p->conflicts : 0; }
const char *np_node_name(const np_node *n) { return n ? n->name : NULL; }
const char *np_node_text(const np_node *n) { return n ? n->text : NULL; }
size_t np_node_text_length(const np_node *n) { return n ? n->text_length : 0; }
size_t np_node_start(const np_node *n) { return n ? n->start : 0; }
size_t np_node_end(const np_node *n) { return n ? n->end : 0; }
size_t np_node_child_count(const np_node *n) { return n ? n->child_count : 0; }
const np_node *np_node_child(const np_node *n, size_t i) { return n && i < n->child_count ? n->children[i] : NULL; }

typedef struct { char *text; size_t length, capacity; int failed; } json_buffer;

static void append(json_buffer *buffer, const char *text, size_t length)
{
    if (buffer->failed) return;
    if (length >= SIZE_MAX - buffer->length) { buffer->failed = 1; return; }
    size_t needed = buffer->length + length + 1;
    if (needed > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *replacement = realloc(buffer->text, capacity);
        if (!replacement) { buffer->failed = 1; return; }
        buffer->text = replacement; buffer->capacity = capacity;
    }
    memcpy(buffer->text + buffer->length, text, length);
    buffer->length += length;
    buffer->text[buffer->length] = '\0';
}

static void json_string(json_buffer *buffer, const char *text, size_t length)
{
    append(buffer, "\"", 1);
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char escape[] = {'\\', (char)c}; append(buffer, escape, 2);
        } else if (c < 32) {
            char escape[7]; snprintf(escape, sizeof escape, "\\u%04x", c); append(buffer, escape, 6);
        } else append(buffer, text + i, 1);
    }
    append(buffer, "\"", 1);
}

const char *np_parser_json(np_parser *parser)
{
    if (!parser || !parser->root) return NULL;
    if (parser->json) return parser->json;
    typedef struct { const np_node *node; size_t child; int opened; } frame;
    size_t capacity = 64, count = 1;
    frame *stack = calloc(capacity, sizeof *stack);
    json_buffer buffer = {0};
    if (!stack) goto fail;
    stack[0].node = parser->root;
    while (count && !buffer.failed) {
        frame *current = &stack[count - 1];
        const np_node *node = current->node;
        if (!node || !node->name) { buffer.failed = 1; break; }
        if (current->child > node->child_count) { buffer.failed = 1; break; }
        if (!current->opened) {
            append(&buffer, "{\"name\":", 8);
            json_string(&buffer, node->name, strlen(node->name));
            char span[96];
            int length = snprintf(span, sizeof span, ",\"start\":%zu,\"end\":%zu", node->start, node->end);
            if (length <= 0 || (size_t)length >= sizeof span) { buffer.failed = 1; break; }
            append(&buffer, span, (size_t)length);
            if (node->text) {
                if (!node->text_length && node->text[0]) { buffer.failed = 1; break; }
                append(&buffer, ",\"text\":", 8); json_string(&buffer, node->text, node->text_length);
            }
            if (node->child_count && !node->children) { buffer.failed = 1; break; }
            append(&buffer, ",\"children\":[", 13);
            current->opened = 1;
        }
        if (current->child == node->child_count) { append(&buffer, "]}", 2); --count; continue; }
        if (current->child) append(&buffer, ",", 1);
        const np_node *child = node->children[current->child++];
        if (!child) { buffer.failed = 1; break; }
        if (count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof *stack) { buffer.failed = 1; break; }
            capacity *= 2;
            frame *replacement = realloc(stack, capacity * sizeof *stack);
            if (!replacement) { buffer.failed = 1; break; }
            stack = replacement;
        }
        stack[count++] = (frame){child, 0, 0};
    }
    free(stack);
    if (buffer.failed) goto fail;
    parser->json = buffer.text;
    return parser->json;
fail:
    free(buffer.text);
    snprintf(parser->error, sizeof parser->error, "out of memory while serializing tree");
    return NULL;
}
