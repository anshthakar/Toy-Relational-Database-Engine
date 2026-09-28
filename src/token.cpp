#include "token.h"

namespace reldb {

    const char* TokenTypeName(TokenType type) {
        switch (type) {
            case TokenType::CREATE: return "CREATE";
            case TokenType::TABLE: return "TABLE";
            case TokenType::INSERT: return "INSERT";
            case TokenType::INTO: return "INTO";
            case TokenType::VALUES: return "VALUES";
            case TokenType::SELECT: return "SELECT";
            case TokenType::FROM: return "FROM";
            case TokenType::WHERE: return "WHERE";
            case TokenType::JOIN: return "JOIN";
            case TokenType::ON: return "ON";
            case TokenType::AS: return "AS";
            case TokenType::AND: return "AND";
            case TokenType::OR: return "OR";
            case TokenType::PRIMARY: return "PRIMARY";
            case TokenType::KEY: return "KEY";
            case TokenType::INTEGER_TYPE: return "INTEGER";
            case TokenType::TEXT_TYPE: return "TEXT";
            case TokenType::IDENTIFIER: return "identifier";
            case TokenType::INTEGER_LITERAL: return "integer literal";
            case TokenType::STRING_LITERAL: return "string literal";
            case TokenType::STAR: return "'*'";
            case TokenType::COMMA: return "','";
            case TokenType::DOT: return "'.'";
            case TokenType::SEMICOLON: return "';'";
            case TokenType::LPAREN: return "'('";
            case TokenType::RPAREN: return "')'";
            case TokenType::EQ: return "'='";
            case TokenType::NEQ: return "'!='";
            case TokenType::LT: return "'<'";
            case TokenType::LTE: return "'<='";
            case TokenType::GT: return "'>'";
            case TokenType::GTE: return "'>='";
            case TokenType::END_OF_INPUT: return "end of input";
        }
        return "<unknown token>";
    }

}  // namespace reldb