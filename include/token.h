#pragma once

#include <cstdint>
#include <string>

namespace reldb {

    // Every token the lexer can produce. Keywords are their own token types
    // (not a generic IDENTIFIER that the parser string-compares later) so a
    // misspelled keyword shows up as an identifier in the right place in an
    // error message, and the parser never has to re-check spelling.
    enum class TokenType {
        // Keywords
        CREATE,
        TABLE,
        INSERT,
        INTO,
        VALUES,
        SELECT,
        FROM,
        WHERE,
        JOIN,
        ON,
        AS,
        AND,
        OR,
        PRIMARY,
        KEY,
        INTEGER_TYPE,  // the type keyword "INTEGER", distinct from an integer
                       // literal token below
        TEXT_TYPE,     // the type keyword "TEXT"

        // Literals and identifiers
        IDENTIFIER,
        INTEGER_LITERAL,
        STRING_LITERAL,

        // Punctuation / operators
        STAR,       // *
        COMMA,      // ,
        DOT,        // .
        SEMICOLON,  // ;
        LPAREN,     // (
        RPAREN,     // )
        EQ,         // =
        NEQ,        // !=
        LT,         // <
        LTE,        // <=
        GT,         // >
        GTE,        // >=

        END_OF_INPUT,
      };

    // One lexical token. `line`/`column` are 1-based and exist purely for
    // error messages — the parser never branches on them.
    struct Token {
        TokenType type;
        std::string text;    // the raw matched text (identifier name, literal
        // spelling, operator symbol) — empty for tokens
        // with no meaningful text, like END_OF_INPUT
        int64_t int_value = 0;     // populated only for INTEGER_LITERAL
        std::string string_value;  // populated only for STRING_LITERAL (the
        // unescaped contents, quotes stripped)
        int line = 0;
        int column = 0;
    };

    // Human-readable name for error messages and test assertions — not the
    // same as `text`, which is empty for punctuation/keyword tokens whose
    // text is implied by their type.
    const char* TokenTypeName(TokenType type);

}  // namespace reldb