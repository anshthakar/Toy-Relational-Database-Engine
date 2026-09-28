#pragma once

#include <stdexcept>
#include <vector>

#include "ast.h"
#include "token.h"

namespace reldb {

// Thrown for any syntax error: wrong token where a specific one was
// expected, or a statement that doesn't start with a recognized keyword.
// Always carries the offending token's position, never just "parse
// failed" with no location — a parser you can't debug from its own error
// messages isn't worth hand-writing.
struct ParseError : std::runtime_error {
  ParseError(const std::string& msg, int line, int column)
      : std::runtime_error(msg + " at line " + std::to_string(line) +
                            ", column " + std::to_string(column)),
        line(line),
        column(column) {}
  int line;
  int column;
};

// Hand-written recursive-descent parser (see docs/milestone-3.md for the
// full grammar) — one method per grammar rule, each consuming exactly the
// tokens that rule's production covers and returning control to its
// caller, no backtracking anywhere. The grammar was kept small
// specifically so a single token of lookahead (Peek()) is always enough
// to know which production applies; nothing here needs arbitrary
// lookahead or a separate tokens-to-try list.
//
// Purely syntactic: this class checks that the token stream matches the
// grammar and builds an AST from it. It does NOT check that a referenced
// table or column exists, that types agree, or that CREATE TABLE declared
// exactly one PRIMARY KEY column — those are semantic checks that need a
// schema to check against, which belongs to milestone 4's execution
// layer.
class Parser {
 public:
  explicit Parser(std::vector<Token> tokens);

  // Parses exactly one statement and confirms nothing but an optional
  // trailing ';' follows it — multiple statements per call aren't
  // supported (see docs/milestone-3.md). Throws ParseError on any syntax
  // problem.
  std::unique_ptr<Statement> ParseStatement();

 private:
  const Token& Peek(int offset = 0) const;
  const Token& Advance();
  bool Check(TokenType type) const;
  bool Match(TokenType type);
  const Token& Expect(TokenType type, const std::string& what);
  [[noreturn]] void Error(const std::string& what) const;

  std::unique_ptr<Statement> ParseCreateTable();
  std::unique_ptr<Statement> ParseInsert();
  std::unique_ptr<Statement> ParseSelect();

  ColumnDef ParseColumnDef();
  ColumnType ParseTypeName();
  Literal ParseLiteral();
  std::string ParseIdent();

  TableRef ParseTableRef();
  JoinClause ParseJoinClause();
  SelectItem ParseSelectItem();

  std::unique_ptr<Expr> ParseExpr();        // OR level (lowest precedence)
  std::unique_ptr<Expr> ParseAndExpr();
  std::unique_ptr<Expr> ParsePrimaryExpr();  // parenthesized expr | comparison
  std::unique_ptr<Expr> ParseComparison();
  std::unique_ptr<Expr> ParseValue();       // column_ref | literal
  ComparisonOp ParseComparisonOp();

  std::vector<Token> tokens_;
  size_t pos_ = 0;
};

// Convenience wrapper: lex + parse one statement in one call.
std::unique_ptr<Statement> ParseSQL(const std::string& sql);

}  // namespace reldb