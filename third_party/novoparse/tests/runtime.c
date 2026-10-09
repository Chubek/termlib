#include "Arithmetic.h"
#include "JSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Keep test assertions active in Release builds. */
#ifdef NDEBUG
#undef NDEBUG
#undef assert
#include <assert.h>
#endif

int main(void)
{
    char error[512];
    np_parser *parser = Arithmetic_create(error, sizeof error);
    assert(parser);
    assert(np_parser_parse(parser, "1+2*3", 5) == NP_SUCCESS);
    const np_node *root = np_parser_root(parser);
    assert(strcmp(np_node_name(root), "Add") == 0);
    assert(strcmp(np_node_name(np_node_child(root, 2)), "Multiply") == 0);
    assert(np_node_start(root) == 0 && np_node_end(root) == 5);
    assert(np_node_child(root, 3) == NULL);
    assert(!np_parser_ambiguous(parser) && np_parser_conflicts(parser) == 0);
    assert(strstr(np_parser_json(parser), "\"text\":\"3\""));
    assert(np_parser_parse(parser, "1+", 2) == NP_ERROR_SYNTAX);
    assert(np_parser_root(parser) == NULL && np_parser_json(parser) == NULL);
    assert(np_parser_position(parser) == 2);
    const char bounded[] = {'4', '2', '!'};
    assert(np_parser_parse(parser, bounded, 2) == NP_SUCCESS);
    assert(np_node_text_length(np_node_child(np_parser_root(parser), 0)) == 2);
    assert(np_parser_parse(parser, bounded, 3) == NP_ERROR_SYNTAX);
    char owned_input[] = "78";
    assert(np_parser_parse(parser, owned_input, 2) == NP_SUCCESS);
    owned_input[0] = '0';
    assert(strcmp(np_node_text(np_node_child(np_parser_root(parser), 0)), "78") == 0);
    np_parser_destroy(parser);

    parser = JSON_create(error, sizeof error);
    assert(parser);
    const char *json = "\n {\"hello\": [1, true, null, \"world\"]} \t";
    assert(np_parser_parse(parser, json, strlen(json)) == NP_SUCCESS);
    assert(!np_parser_ambiguous(parser));
    assert(strstr(np_parser_json(parser), "\"name\":\"Object\""));
    assert(np_parser_parse(parser, "[1,]", 4) == NP_ERROR_SYNTAX);
    np_parser_destroy(parser);

    const np_symbol_spec symbols[] = {{NP_NONTERMINAL, "S", "", 0, NP_ASSOC_NONE}, {NP_LITERAL, "X", "x", 0, NP_ASSOC_NONE}};
    const int body[] = {1};
    const np_production_spec productions[] = {{0, body, 1, "", 0, NP_ASSOC_NONE}, {0, body, 1, "", 0, NP_ASSOC_NONE}};
    np_grammar_spec spec = {"Test", symbols, 2, productions, 2, 0, NP_SKIP_NONE, "", NP_REJECT, ""};
    parser = np_parser_create(&spec, error, sizeof error);
    assert(parser);
    assert(np_parser_parse(parser, "x", 1) == NP_ERROR_AMBIGUOUS);
    assert(np_parser_ambiguous(parser));
    np_parser_destroy(parser);
    spec.disambiguation = NP_FIRST;
    parser = np_parser_create(&spec, error, sizeof error);
    assert(parser && np_parser_parse(parser, "x", 1) == NP_SUCCESS);
    assert(np_parser_ambiguous(parser));
    np_parser_destroy(parser);
    spec.disambiguation = NP_REJECT;
    spec.rewrites = "(rewrite (rules (remove-duplicate-productions)))";
    parser = np_parser_create(&spec, error, sizeof error);
    assert(parser && np_parser_parse(parser, "x", 1) == NP_SUCCESS);
    assert(!np_parser_ambiguous(parser));
    np_parser_destroy(parser);
    spec.start = 99;
    assert(!np_parser_create(&spec, error, sizeof error) && *error);
    assert(!np_parser_create(NULL, error, sizeof error));
    np_parser_destroy(NULL);
    // Hardening: NULL/empty inputs, out-of-range descriptors, NULL names.
    spec.start = 0;
    spec.rewrites = "";
    parser = Arithmetic_create(error, sizeof error);
    assert(parser);
    assert(np_parser_parse(parser, NULL, 1) == NP_ERROR_SYNTAX);
    assert(np_parser_parse(NULL, "1", 1) == NP_ERROR_GRAMMAR);
    assert(np_parser_parse(parser, NULL, 0) == NP_ERROR_SYNTAX);
    assert(np_parser_root(NULL) == NULL && np_parser_json(NULL) == NULL);
    assert(np_node_child(NULL, 0) == NULL && np_node_name(NULL) == NULL);
    np_parser_destroy(parser);
    const int bad_body[] = {99};
    const np_production_spec bad_productions[] = {{0, bad_body, 1, "", 0, NP_ASSOC_NONE}};
    np_grammar_spec bad = {"Test", symbols, 2, bad_productions, 1, 0, NP_SKIP_NONE, "", NP_REJECT, ""};
    assert(!np_parser_create(&bad, error, sizeof error) && *error);
    const np_symbol_spec bad_symbols[] = {{NP_NONTERMINAL, NULL, "", 0, NP_ASSOC_NONE}, {NP_LITERAL, "X", "x", 0, NP_ASSOC_NONE}};
    bad.symbols = bad_symbols;
    bad.productions = productions;
    bad.production_count = 2;
    assert(!np_parser_create(&bad, error, sizeof error) && *error);
    puts("C runtime checks passed");
    return 0;
}
