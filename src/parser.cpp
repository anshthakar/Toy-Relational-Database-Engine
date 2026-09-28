#include "parser.h"

#include "lexer.h"

namespace reldb {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::Peek(int offset) const {
  size_t idx = pos_ + static_cast<size_t>(offset);
  if (idx >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[idx];
}

const Token& Parser::Advance() {
  const Token& t = Peek();
  if (pos_ < tokens_.size() - 1) pos_++;
  return t;
}

bool Parser::Check(TokenType type) const { return Peek().type == type; }

bool Parser::Match(TokenType type) {
  if (!Check(type)) return false;
  Advance();
  return true;
}

const Token& Parser::Expect(TokenType type, const std::string& what) {
  if (!Check(type)) {
    Error("expected " + what + " but found " +
          TokenTypeName(Peek().type) +
          (Peek().text.empty() ? "" : (" ('" + Peek().text + "')")));
  }
  return Advance();
}

void Parser::Error(const std::string& what) const {
  throw ParseError(what, Peek().line, Peek().column);
}

std::unique_ptr<Statement> Parser::ParseStatement() {
  std::unique_ptr<Statement> stmt;
  switch (Peek().type) {
    case TokenType::CREATE:
      stmt = ParseCreateTable();
      break;
    case TokenType::INSERT:
      stmt = ParseInsert();
      break;
    case TokenType::SELECT:
      stmt = ParseSelect();
      break;
    default:
      Error("expected a statement starting with CREATE, INSERT, or SELECT, "
            "found " +
            std::string(TokenTypeName(Peek().type)));
  }

  Match(TokenType::SEMICOLON);  // optional trailing ';'
  Expect(TokenType::END_OF_INPUT,
         "end of statement (only one statement is supported per parse call)");
  return stmt;
}

// ---------- CREATE TABLE ----------

std::unique_ptr<Statement> Parser::ParseCreateTable() {
  Expect(TokenType::CREATE, "CREATE");
  Expect(TokenType::TABLE, "TABLE");

  auto stmt = std::make_unique<CreateTableStmt>();
  stmt->table_name = ParseIdent();

  Expect(TokenType::LPAREN, "'(' to start the column list");
  stmt->columns.push_back(ParseColumnDef());
  while (Match(TokenType::COMMA)) {
    stmt->columns.push_back(ParseColumnDef());
  }
  Expect(TokenType::RPAREN, "')' to close the column list");

  return stmt;
}

ColumnDef Parser::ParseColumnDef() {
  ColumnDef def;
  def.name = ParseIdent();
  def.type = ParseTypeName();
  if (Match(TokenType::PRIMARY)) {
    Expect(TokenType::KEY, "KEY after PRIMARY");
    def.is_primary_key = true;
  }
  return def;
}

ColumnType Parser::ParseTypeName() {
  if (Match(TokenType::INTEGER_TYPE)) return ColumnType::kInteger;
  if (Match(TokenType::TEXT_TYPE)) return ColumnType::kText;
  Error("expected a column type (INTEGER or TEXT)");
}

// ---------- INSERT ----------

std::unique_ptr<Statement> Parser::ParseInsert() {
  Expect(TokenType::INSERT, "INSERT");
  Expect(TokenType::INTO, "INTO");

  auto stmt = std::make_unique<InsertStmt>();
  stmt->table_name = ParseIdent();

  if (Match(TokenType::LPAREN)) {
    stmt->columns.push_back(ParseIdent());
    while (Match(TokenType::COMMA)) {
      stmt->columns.push_back(ParseIdent());
    }
    Expect(TokenType::RPAREN, "')' to close the column list");
  }

  Expect(TokenType::VALUES, "VALUES");
  Expect(TokenType::LPAREN, "'(' to start the value list");
  stmt->values.push_back(ParseLiteral());
  while (Match(TokenType::COMMA)) {
    stmt->values.push_back(ParseLiteral());
  }
  Expect(TokenType::RPAREN, "')' to close the value list");

  return stmt;
}

Literal Parser::ParseLiteral() {
  if (Check(TokenType::INTEGER_LITERAL)) {
    Literal lit;
    lit.kind = LiteralKind::kInteger;
    lit.int_value = Advance().int_value;
    return lit;
  }
  if (Check(TokenType::STRING_LITERAL)) {
    Literal lit;
    lit.kind = LiteralKind::kText;
    lit.text_value = Advance().string_value;
    return lit;
  }
  Error("expected a literal value (an integer or a quoted string)");
}

std::string Parser::ParseIdent() {
  return Expect(TokenType::IDENTIFIER, "an identifier").text;
}

// ---------- SELECT ----------

std::unique_ptr<Statement> Parser::ParseSelect() {
  Expect(TokenType::SELECT, "SELECT");

  auto stmt = std::make_unique<SelectStmt>();
  if (Match(TokenType::STAR)) {
    stmt->is_star = true;
  } else {
    stmt->select_items.push_back(ParseSelectItem());
    while (Match(TokenType::COMMA)) {
      stmt->select_items.push_back(ParseSelectItem());
    }
  }

  Expect(TokenType::FROM, "FROM");
  stmt->from = ParseTableRef();

  while (Check(TokenType::JOIN)) {
    stmt->joins.push_back(ParseJoinClause());
  }

  if (Match(TokenType::WHERE)) {
    stmt->where = ParseExpr();
  }

  return stmt;
}

SelectItem Parser::ParseSelectItem() {
  SelectItem item;
  std::string first = ParseIdent();
  if (Match(TokenType::DOT)) {
    item.table = first;
    item.column = ParseIdent();
  } else {
    item.column = first;
  }
  if (Match(TokenType::AS)) {
    item.alias = ParseIdent();
  }
  return item;
}

TableRef Parser::ParseTableRef() {
  TableRef ref;
  ref.table = ParseIdent();
  if (Match(TokenType::AS)) {
    ref.alias = ParseIdent();
  }
  return ref;
}

JoinClause Parser::ParseJoinClause() {
  Expect(TokenType::JOIN, "JOIN");
  JoinClause clause;
  clause.table = ParseTableRef();
  Expect(TokenType::ON, "ON");
  clause.on_condition = ParseExpr();
  return clause;
}

// Three precedence levels, lowest to highest: OR, AND, comparison

std::unique_ptr<Expr> Parser::ParseExpr() {
  auto left = ParseAndExpr();
  while (Match(TokenType::OR)) {
    auto right = ParseAndExpr();
    auto node = std::make_unique<OrExpr>();
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParseAndExpr() {
  auto left = ParsePrimaryExpr();
  while (Match(TokenType::AND)) {
    auto right = ParsePrimaryExpr();
    auto node = std::make_unique<AndExpr>();
    node->left = std::move(left);
    node->right = std::move(right);
    left = std::move(node);
  }
  return left;
}

std::unique_ptr<Expr> Parser::ParsePrimaryExpr() {
  if (Match(TokenType::LPAREN)) {
    auto inner = ParseExpr();
    Expect(TokenType::RPAREN, "')' to close the parenthesized expression");
    return inner;
  }
  return ParseComparison();
}

std::unique_ptr<Expr> Parser::ParseComparison() {
  auto left = ParseValue();
  ComparisonOp op = ParseComparisonOp();
  auto right = ParseValue();

  auto node = std::make_unique<ComparisonExpr>();
  node->op = op;
  node->left = std::move(left);
  node->right = std::move(right);
  return node;
}

ComparisonOp Parser::ParseComparisonOp() {
  switch (Peek().type) {
    case TokenType::EQ: Advance(); return ComparisonOp::kEq;
    case TokenType::NEQ: Advance(); return ComparisonOp::kNeq;
    case TokenType::LT: Advance(); return ComparisonOp::kLt;
    case TokenType::LTE: Advance(); return ComparisonOp::kLte;
    case TokenType::GT: Advance(); return ComparisonOp::kGt;
    case TokenType::GTE: Advance(); return ComparisonOp::kGte;
    default:
      Error("expected a comparison operator (=, !=, <, <=, >, or >=)");
  }
}

std::unique_ptr<Expr> Parser::ParseValue() {
  if (Check(TokenType::IDENTIFIER)) {
    auto node = std::make_unique<ColumnRefExpr>();
    std::string first = ParseIdent();
    if (Match(TokenType::DOT)) {
      node->table = first;
      node->column = ParseIdent();
    } else {
      node->column = first;
    }
    return node;
  }
  if (Check(TokenType::INTEGER_LITERAL) || Check(TokenType::STRING_LITERAL)) {
    auto node = std::make_unique<LiteralExpr>();
    node->value = ParseLiteral();
    return node;
  }
  Error("expected a column reference or a literal value");
}

// ---------- Convenience entry point ----------

std::unique_ptr<Statement> ParseSQL(const std::string& sql) {
  Lexer lexer(sql);
  Parser parser(lexer.Tokenize());
  return parser.ParseStatement();
}

}  // namespace reldb