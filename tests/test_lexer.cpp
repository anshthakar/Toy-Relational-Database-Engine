// Lexer unit tests: token-by-token checks on a handful of representative
// inputs, plus the error cases (unterminated string, unrecognized
// character) that the lexer is responsible for catching before the parser
// ever sees a malformed token stream.

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "lexer.h"

using namespace reldb;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      throw std::runtime_error(std::string("CHECK failed: ") + #cond +   \
                                " at " + __FILE__ + ":" +                \
                                std::to_string(__LINE__));                \
    }                                                                    \
  } while (0)

namespace {

std::vector<Token> Lex(const std::string& src) {
  Lexer lexer(src);
  return lexer.Tokenize();
}

void TestKeywordsAreCaseInsensitive() {
  auto tokens = Lex("select FROM Where");
  CHECK(tokens.size() == 4);  // 3 keywords + EOF
  CHECK(tokens[0].type == TokenType::SELECT);
  CHECK(tokens[1].type == TokenType::FROM);
  CHECK(tokens[2].type == TokenType::WHERE);
  CHECK(tokens[3].type == TokenType::END_OF_INPUT);
}

void TestIdentifiersPreserveCase() {
  auto tokens = Lex("Users users USERS");
  CHECK(tokens[0].type == TokenType::IDENTIFIER && tokens[0].text == "Users");
  CHECK(tokens[1].type == TokenType::IDENTIFIER && tokens[1].text == "users");
  CHECK(tokens[2].type == TokenType::IDENTIFIER && tokens[2].text == "USERS");
}

void TestIntegerLiteral() {
  auto tokens = Lex("42");
  CHECK(tokens[0].type == TokenType::INTEGER_LITERAL);
  CHECK(tokens[0].int_value == 42);
}

void TestStringLiteralWithEscapedQuote() {
  auto tokens = Lex("'it''s'");
  CHECK(tokens[0].type == TokenType::STRING_LITERAL);
  CHECK(tokens[0].string_value == "it's");
}

void TestOperators() {
  auto tokens = Lex("= != < <= > >=");
  std::vector<TokenType> expected = {
      TokenType::EQ, TokenType::NEQ, TokenType::LT,
      TokenType::LTE, TokenType::GT, TokenType::GTE,
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(tokens[i].type == expected[i]);
  }
}

void TestPunctuation() {
  auto tokens = Lex("* , . ; ( )");
  std::vector<TokenType> expected = {
      TokenType::STAR, TokenType::COMMA,  TokenType::DOT,
      TokenType::SEMICOLON, TokenType::LPAREN, TokenType::RPAREN,
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK(tokens[i].type == expected[i]);
  }
}

void TestFullStatementTokenizes() {
  auto tokens = Lex(
      "SELECT a.id, b.name FROM a JOIN b ON a.id = b.a_id "
      "WHERE a.id > 10 AND b.name = 'x';");
  // Spot-check positions rather than every token
  CHECK(tokens.front().type == TokenType::SELECT);
  CHECK(tokens.back().type == TokenType::END_OF_INPUT);
  bool saw_join = false, saw_where = false, saw_string = false;
  for (auto& t : tokens) {
    if (t.type == TokenType::JOIN) saw_join = true;
    if (t.type == TokenType::WHERE) saw_where = true;
    if (t.type == TokenType::STRING_LITERAL && t.string_value == "x") {
      saw_string = true;
    }
  }
  CHECK(saw_join && saw_where && saw_string);
}

void TestUnterminatedStringThrows() {
  bool threw = false;
  try {
    Lex("'abc");
  } catch (const LexError&) {
    threw = true;
  }
  CHECK(threw);
}

void TestUnexpectedCharacterThrows() {
  bool threw = false;
  try {
    Lex("SELECT @ FROM t");
  } catch (const LexError&) {
    threw = true;
  }
  CHECK(threw);
}

void TestLoneBangThrows() {
  bool threw = false;
  try {
    Lex("a ! b");
  } catch (const LexError&) {
    threw = true;
  }
  CHECK(threw);
}

void TestLineAndColumnTracking() {
  auto tokens = Lex("SELECT\n  x");
  CHECK(tokens[1].line == 2);
  CHECK(tokens[1].column == 3);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"KeywordsAreCaseInsensitive", TestKeywordsAreCaseInsensitive},
      {"IdentifiersPreserveCase", TestIdentifiersPreserveCase},
      {"IntegerLiteral", TestIntegerLiteral},
      {"StringLiteralWithEscapedQuote", TestStringLiteralWithEscapedQuote},
      {"Operators", TestOperators},
      {"Punctuation", TestPunctuation},
      {"FullStatementTokenizes", TestFullStatementTokenizes},
      {"UnterminatedStringThrows", TestUnterminatedStringThrows},
      {"UnexpectedCharacterThrows", TestUnexpectedCharacterThrows},
      {"LoneBangThrows", TestLoneBangThrows},
      {"LineAndColumnTracking", TestLineAndColumnTracking},
  };

  int failures = 0;
  for (auto& [name, fn] : tests) {
    try {
      fn();
      std::printf("[PASS] %s\n", name.c_str());
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", name.c_str(), e.what());
      failures++;
    }
  }

  std::printf("\n%zu tests, %d failed\n", tests.size(), failures);
  return failures == 0 ? 0 : 1;
}