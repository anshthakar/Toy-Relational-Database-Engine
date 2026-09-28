#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "token.h"

namespace reldb {

    // Thrown for any character-level problem: an unterminated string literal,
    // an unrecognized character, a malformed number. Carries line/column so
    // the caller can point at the exact spot, same spirit as the parser's own
    // ParseError.
    struct LexError : std::runtime_error {
        LexError(const std::string& msg, int line, int column)
            : std::runtime_error(msg + " at line " + std::to_string(line) +
                                  ", column " + std::to_string(column)),
              line(line),
              column(column) {}
        int line;
        int column;
    };

    // Converts a full SQL statement string into a flat list of tokens, ending
    // with a single END_OF_INPUT token (so the parser never has to
    // special-case "ran off the end of the vector" separately from "hit the
    // natural end of the statement").
    //
    // Deliberately simple: single-pass, no lookahead beyond one character,
    // case-insensitive keywords (SQL keywords conventionally are), and no
    // support for comments (-- or /* */) or escaped-quote handling inside
    // string literals beyond a doubled quote ('' -> ') — the one escape form
    // standard SQL actually requires.
    class Lexer {
    public:
        explicit Lexer(std::string source);

        // Tokenizes the entire input in one call and returns the result. Not
        // designed for incremental/streaming use — the whole statement is
        // expected to fit in memory, same simplification the WAL's full-page
        // logging makes for pages.
        std::vector<Token> Tokenize();

    private:
        char Peek(int offset = 0) const;
        char Advance();
        bool IsAtEnd() const;
        void SkipWhitespace();

        Token MakeToken(TokenType type, const std::string& text);
        Token LexIdentifierOrKeyword();
        Token LexNumber();
        Token LexString();

        std::string source_;
        size_t pos_ = 0;
        int line_ = 1;
        int column_ = 1;
    };

}  // namespace reldb