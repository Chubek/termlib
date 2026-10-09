#ifndef NOVOPARSE_HPP
#define NOVOPARSE_HPP
#include <NovoParse.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace novoparse {
class ParseError : public std::runtime_error {
public:
    np_status status;
    size_t position, line, column;
    ParseError(np_parser *parser, np_status code)
        : std::runtime_error(np_parser_error(parser)), status(code),
          position(np_parser_position(parser)), line(np_parser_line(parser)),
          column(np_parser_column(parser)) {}
};

class Parser {
    np_parser *parser_ = nullptr;
public:
    explicit Parser(const np_grammar_spec &spec) {
        char error[512] = {};
        parser_ = np_parser_create(&spec, error, sizeof error);
        if (!parser_) throw std::runtime_error(error);
    }
    virtual ~Parser() { np_parser_destroy(parser_); }
    Parser(const Parser &) = delete;
    Parser &operator=(const Parser &) = delete;
    Parser(Parser &&other) noexcept : parser_(other.parser_) { other.parser_ = nullptr; }
    Parser &operator=(Parser &&other) noexcept {
        if (this != &other) { np_parser_destroy(parser_); parser_ = other.parser_; other.parser_ = nullptr; }
        return *this;
    }
    std::string parse(const std::string &text) {
        auto status = np_parser_parse(parser_, text.data(), text.size());
        if (status != NP_SUCCESS) throw ParseError(parser_, status);
        auto json = np_parser_json(parser_);
        if (!json) throw std::runtime_error(np_parser_error(parser_));
        return json;
    }
    bool ambiguous() const { return np_parser_ambiguous(parser_) != 0; }
    size_t conflicts() const { return np_parser_conflicts(parser_); }
    const np_node *root() const { return np_parser_root(parser_); }
};
}
#endif
