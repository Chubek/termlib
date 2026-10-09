#include "Arithmetic.hpp"
#include <cassert>
#include <string>
#include <utility>
#ifdef NDEBUG
#undef NDEBUG
#undef assert
#include <cassert>
#endif

int main()
{
    ArithmeticParser parser;
    assert(parser.parse("1+2*3").find("\"name\":\"Multiply\"") != std::string::npos);
    assert(!parser.ambiguous());
    ArithmeticParser moved(std::move(parser));
    assert(moved.parse("1/2").find("\"name\":\"Divide\"") != std::string::npos);
    try { moved.parse("1+\n"); assert(false); }
    catch (const novoparse::ParseError &error) {
        assert(error.status == NP_ERROR_SYNTAX && error.position == 3 && error.line == 2);
    }
}
