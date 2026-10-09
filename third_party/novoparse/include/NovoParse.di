module NovoParse;

import std.json : JSONValue;

struct SymbolSpec
{
    int kind;
    const(char)* name;
    const(char)* value;
    int precedence;
    int associativity;
}
struct ProductionSpec
{
    int head;
    const(int)* body;
    size_t body_length;
    const(char)* label;
    int precedence;
    int associativity;
}
struct GrammarSpec
{
    const(char)* name;
    const(SymbolSpec)* symbols;
    size_t symbol_count;
    const(ProductionSpec)* productions;
    size_t production_count;
    int start;
    int skip_kind;
    const(char)* skip;
    int disambiguation;
    const(char)* rewrites;
}
extern(C)
{
    void* np_parser_create(const(GrammarSpec)* spec, char* error, size_t error_size);
    void np_parser_destroy(void* parser);
    int np_parser_parse(void* parser, const(char)* input, size_t length);
    const(char)* np_parser_json(void* parser);
    const(char)* np_parser_error(const(void)* parser);
    size_t np_parser_position(const(void)* parser);
    size_t np_parser_line(const(void)* parser);
    size_t np_parser_column(const(void)* parser);
    int np_parser_ambiguous(const(void)* parser);
    size_t np_parser_conflicts(const(void)* parser);
}
class ParseError : Exception
{
    int status;
    size_t position, line, column;
    this(void* parser, int status);
}
class Parser
{
    private void* handle;
    this(const(GrammarSpec)* spec);
    ~this();
    void close();
    private void requireOpen() const;
    JSONValue parse(string input);
    bool ambiguous() const;
    size_t conflicts() const;
}
