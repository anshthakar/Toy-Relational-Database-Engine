#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace reldb {

// --- Documented grammar subset (see docs/milestone-3.md for the full
// EBNF and the list of what's deliberately NOT supported) ---
// CREATE TABLE, INSERT, and SELECT with WHERE and a chain of bare
// JOIN ... ON clauses (inner join only). WHERE is boolean-only: AND/OR
// combining comparisons between a column reference and a literal (or two
// column references) — no arithmetic, no NOT, no IN/LIKE/BETWEEN.

// --- Types ---

enum class ColumnType {
  kInteger,  // maps to the storage engine's int64_t
  kText,     // maps to a fixed-capacity string, sized in milestone 4
};

const char* ColumnTypeName(ColumnType type);

// --- Values (literals appearing in SQL text) ---

enum class LiteralKind { kInteger, kText };

struct Literal {
  LiteralKind kind;
  int64_t int_value = 0;
  std::string text_value;
};

// --- Expressions ---
//
// One flat hierarchy rather than separate "value expression" and "boolean
// expression" base classes. The grammar is what actually restricts a
// comparison's operands to column references and literals, and a WHERE
// clause's root to a boolean combinator — the AST itself doesn't encode
// that restriction with separate C++ types, the same way LeafCell and
// InternalCell in btree_node.h don't have a common base: it would add a
// layer of indirection for a distinction only the parser needs to
// enforce, once, at construction time.
enum class ExprKind { kColumnRef, kLiteral, kComparison, kAnd, kOr };

enum class ComparisonOp { kEq, kNeq, kLt, kLte, kGt, kGte };

struct Expr {
  explicit Expr(ExprKind k) : kind(k) {}
  virtual ~Expr() = default;
  ExprKind kind;
};

// (ident ".")? ident — the table qualifier is std::nullopt when the
// column was written unqualified ("id", not "orders.id").
struct ColumnRefExpr : Expr {
  ColumnRefExpr() : Expr(ExprKind::kColumnRef) {}
  std::optional<std::string> table;
  std::string column;
};

struct LiteralExpr : Expr {
  LiteralExpr() : Expr(ExprKind::kLiteral) {}
  Literal value;
};

// left/right are each expected (by the parser, not enforced by this
// struct's type) to be a ColumnRefExpr or LiteralExpr — a comparison
// between two boolean subexpressions isn't meaningful SQL and the parser
// never constructs one.
struct ComparisonExpr : Expr {
  ComparisonExpr() : Expr(ExprKind::kComparison) {}
  ComparisonOp op;
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
};

struct AndExpr : Expr {
  AndExpr() : Expr(ExprKind::kAnd) {}
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
};

struct OrExpr : Expr {
  OrExpr() : Expr(ExprKind::kOr) {}
  std::unique_ptr<Expr> left;
  std::unique_ptr<Expr> right;
};

// --- CREATE TABLE ---

struct ColumnDef {
  std::string name;
  ColumnType type;
  bool is_primary_key = false;
};

// --- FROM / JOIN ---

// A single "ident (AS ident)?" appearing after FROM or JOIN.
struct TableRef {
  std::string table;
  std::optional<std::string> alias;
};

// "JOIN table_ref ON expr" — inner join only, no LEFT/RIGHT/OUTER/CROSS
// (see docs/milestone-3.md). `on_condition` is never null; the grammar
// requires ON for every JOIN.
struct JoinClause {
  TableRef table;
  std::unique_ptr<Expr> on_condition;
};

// (ident ".")? ident ("AS" ident)? — one entry in a non-"*" select list.
struct SelectItem {
  std::optional<std::string> table;
  std::string column;
  std::optional<std::string> alias;
};

// --- Statements ---

enum class StatementKind { kCreateTable, kInsert, kSelect };

struct Statement {
  explicit Statement(StatementKind k) : kind(k) {}
  virtual ~Statement() = default;
  StatementKind kind;
};

struct CreateTableStmt : Statement {
  CreateTableStmt() : Statement(StatementKind::kCreateTable) {}
  std::string table_name;
  std::vector<ColumnDef> columns;
};

struct InsertStmt : Statement {
  InsertStmt() : Statement(StatementKind::kInsert) {}
  std::string table_name;
  // Empty means "no explicit column list was given" — INSERT INTO t
  // VALUES (...) rather than INSERT INTO t (a, b) VALUES (...). Resolving
  // that to actual column positions is a semantic step for milestone 4,
  // which needs the table's schema to do it; the parser just records
  // what was (or wasn't) written.
  std::vector<std::string> columns;
  std::vector<Literal> values;
};

struct SelectStmt : Statement {
  SelectStmt() : Statement(StatementKind::kSelect) {}
  bool is_star = false;              // SELECT * — select_items is empty
  std::vector<SelectItem> select_items;
  TableRef from;
  std::vector<JoinClause> joins;     // empty for a single-table query
  std::unique_ptr<Expr> where;       // nullptr if there was no WHERE
};

}  // namespace reldb