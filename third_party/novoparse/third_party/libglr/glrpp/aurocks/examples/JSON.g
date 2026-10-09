grammar JSON

%ATN on
%lexer SCANNERLESS
%start JSON
%language C++
%parser JSONParser.cpp
%runtime JSONParserRuntime.hpp
%AST JSONParserAST.hpp
%build Makefile

    JSON
        = Object
        | Array
        | String
        | Number
        | "true"
        | "false"
        | "null"

    Object
        = "{"
        | "{" Members "}"

    Members
        = Member
        | Members "," Member

    Member
        = String ":" JSON

    Array
        = "["
        | "[" Elements "]"

    Elements
        = JSON
        | Elements "," JSON

    String
        = "\"" StringChar* "\""

    StringChar
        = [\u0020-\u0021]
        | [\u0023-\u005B]
        | [\u005D-\uFFFF]
        | Escape

    Escape
        = "\\\""
        | "\\\\"
        | "\\/"
        | "\\b"
        | "\\f"
        | "\\n"
        | "\\r"
        | "\\t"
        | "\\u" Hex Hex Hex Hex

    Hex
        = [0-9]
        | [a-f]
        | [A-F]

    Number
        = "-"? Integer Fraction? Exponent?

    Integer
        = "0"
        | NonZeroDigit Digit*

    Fraction
        = "." Digit+

    Exponent
        = ("e" | "E") ("+" | "-")? Digit+

    Digit
        = [0-9]

    NonZeroDigit
        = [1-9]

layout
    = [\ \t\n\r]

