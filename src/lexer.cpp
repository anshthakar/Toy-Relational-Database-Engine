#include "lexer.h"

#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace reldb {

namespace {


const std::unordered_map<std::string, TokenType>& KeywordTable() {
  static const std::unordered_map<std::string, TokenType> table = {
      {"CREATE", TokenType::CREATE},
      {"TABLE", TokenType::TABLE},
      {"INSERT", TokenType::INSERT},
      {"INTO", TokenType::INTO},
      {"VALUES", TokenType::VALUES},
      {"SELECT", TokenType::SELECT},
      {"FROM", TokenType::FROM},
      {"WHERE", TokenType::WHERE},
      {"JOIN", TokenType::JOIN},
      {"ON", TokenType::ON},
      {"AS", TokenType::AS},
      {"AND", TokenType::AND},
      {"OR", TokenType::OR},
      {"PRIMARY", TokenType::PRIMARY},
      {"KEY", TokenType::KEY},
      {"INTEGER", TokenType::INTEGER_TYPE},
      {"TEXT", TokenType::TEXT_TYPE},
  };
  return table;
}

std::string ToUpper(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

Lexer::Lexer(std::string source) : source_(std::move(source)) {}

bool Lexer::IsAtEnd() const { return pos_ >= source_.size(); }

char Lexer::Peek(int offset) const {
  size_t idx = pos_ + static_cast<size_t>(offset);
  if (idx >= source_.size()) return '\0';
  return source_[idx];
}

char Lexer::Advance() {
  char c = source_[pos_++];
  if (c == '\n') {
    line_++;
    column_ = 1;
  } else {
    column_++;
  }
  return c;
}

void Lexer::SkipWhitespace() {
  while (!IsAtEnd() && std::isspace(static_cast<unsigned char>(Peek()))) {
    Advance();
  }
}

Token Lexer::MakeToken(TokenType type, const std::string& text) {
  Token t;
  t.type = type;
  t.text = text;
  t.line = line_;
  t.column = column_ - static_cast<int>(text.size());
  return t;
}

Token Lexer::LexIdentifierOrKeyword() {
  int start_line = line_;
  int start_col = column_;
  std::string text;
  while (!IsAtEnd() &&
         (std::isalnum(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
    text.push_back(Advance());
  }
  auto& keywords = KeywordTable();
  auto it = keywords.find(ToUpper(text));
  Token t;
  t.type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;
  t.text = text;
  t.line = start_line;
  t.column = start_col;
  return t;
}

Token Lexer::LexNumber() {
  int start_line = line_;
  int start_col = column_;
  std::string text;

  while (!IsAtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) {
    text.push_back(Advance());
  }
  Token t;
  t.type = TokenType::INTEGER_LITERAL;
  t.text = text;
  t.int_value = std::stoll(text);
  t.line = start_line;
  t.column = start_col;
  return t;
}

Token Lexer::LexString() {
  int start_line = line_;
  int start_col = column_;
  Advance();
  std::string value;
  while (true) {
    if (IsAtEnd()) {
      throw LexError("unterminated string literal", start_line, start_col);
    }
    char c = Peek();
    if (c == '\'') {
      Advance();

      if (Peek() == '\'') {
        value.push_back('\'');
        Advance();
        continue;
      }
      break;  // real closing quote
    }
    value.push_back(Advance());
  }
  Token t;
  t.type = TokenType::STRING_LITERAL;
  t.string_value = value;
  t.text = value;
  t.line = start_line;
  t.column = start_col;
  return t;
}

std::vector<Token> Lexer::Tokenize() {
  std::vector<Token> tokens;

  while (true) {
    SkipWhitespace();
    if (IsAtEnd()) break;

    char c = Peek();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      tokens.push_back(LexIdentifierOrKeyword());
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      tokens.push_back(LexNumber());
      continue;
    }
    if (c == '\'') {
      tokens.push_back(LexString());
      continue;
    }

    int start_line = line_;
    int start_col = column_;
    switch (c) {
      case '*': Advance(); tokens.push_back(MakeToken(TokenType::STAR, "*")); break;
      case ',': Advance(); tokens.push_back(MakeToken(TokenType::COMMA, ",")); break;
      case '.': Advance(); tokens.push_back(MakeToken(TokenType::DOT, ".")); break;
      case ';': Advance(); tokens.push_back(MakeToken(TokenType::SEMICOLON, ";")); break;
      case '(': Advance(); tokens.push_back(MakeToken(TokenType::LPAREN, "(")); break;
      case ')': Advance(); tokens.push_back(MakeToken(TokenType::RPAREN, ")")); break;
      case '=': Advance(); tokens.push_back(MakeToken(TokenType::EQ, "=")); break;
      case '<': {
        Advance();
        if (Peek() == '=') {
          Advance();
          tokens.push_back(MakeToken(TokenType::LTE, "<="));
        } else {
          tokens.push_back(MakeToken(TokenType::LT, "<"));
        }
        break;
      }
      case '>': {
        Advance();
        if (Peek() == '=') {
          Advance();
          tokens.push_back(MakeToken(TokenType::GTE, ">="));
        } else {
          tokens.push_back(MakeToken(TokenType::GT, ">"));
        }
        break;
      }
      case '!': {
        Advance();
        if (Peek() == '=') {
          Advance();
          tokens.push_back(MakeToken(TokenType::NEQ, "!="));
        } else {
          throw LexError("unexpected character '!' (did you mean '!='?)",
                          start_line, start_col);
        }
        break;
      }
      default:
        throw LexError(std::string("unexpected character '") + c + "'",
                        start_line, start_col);
    }
  }

  Token eof;
  eof.type = TokenType::END_OF_INPUT;
  eof.line = line_;
  eof.column = column_;
  tokens.push_back(eof);
  return tokens;
}

}  // namespace reldb