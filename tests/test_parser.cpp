// Parser unit tests: for each supported statement shape, parse it and
// walk the resulting AST checking every field that matters

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "parser.h"

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

void ExpectParseError(const std::string& sql) {
  bool threw = false;
  try {
    ParseSQL(sql);
  } catch (const ParseError&) {
    threw = true;
  }
  CHECK(threw);
}

void TestCreateTableBasic() {
  auto stmt = ParseSQL("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
  CHECK(stmt->kind == StatementKind::kCreateTable);
  auto* ct = static_cast<CreateTableStmt*>(stmt.get());
  CHECK(ct->table_name == "users");
  CHECK(ct->columns.size() == 2);
  CHECK(ct->columns[0].name == "id");
  CHECK(ct->columns[0].type == ColumnType::kInteger);
  CHECK(ct->columns[0].is_primary_key == true);
  CHECK(ct->columns[1].name == "name");
  CHECK(ct->columns[1].type == ColumnType::kText);
  CHECK(ct->columns[1].is_primary_key == false);
}

void TestCreateTableRequiresAtLeastOneColumn() {
  ExpectParseError("CREATE TABLE t ()");
}

void TestCreateTableRejectsUnknownType() {
  ExpectParseError("CREATE TABLE t (id VARCHAR)");
}

void TestInsertWithExplicitColumns() {
  auto stmt = ParseSQL("INSERT INTO users (id, name) VALUES (1, 'alice')");
  CHECK(stmt->kind == StatementKind::kInsert);
  auto* ins = static_cast<InsertStmt*>(stmt.get());
  CHECK(ins->table_name == "users");
  CHECK(ins->columns.size() == 2);
  CHECK(ins->columns[0] == "id");
  CHECK(ins->columns[1] == "name");
  CHECK(ins->values.size() == 2);
  CHECK(ins->values[0].kind == LiteralKind::kInteger);
  CHECK(ins->values[0].int_value == 1);
  CHECK(ins->values[1].kind == LiteralKind::kText);
  CHECK(ins->values[1].text_value == "alice");
}

void TestInsertWithoutColumnList() {
  auto stmt = ParseSQL("INSERT INTO users VALUES (1, 'alice')");
  auto* ins = static_cast<InsertStmt*>(stmt.get());
  CHECK(ins->columns.empty());
  CHECK(ins->values.size() == 2);
}

void TestInsertRejectsMismatchedParens() {
  ExpectParseError("INSERT INTO users VALUES (1, 'alice'");
}

void TestSelectStar() {
  auto stmt = ParseSQL("SELECT * FROM users");
  CHECK(stmt->kind == StatementKind::kSelect);
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->is_star == true);
  CHECK(sel->select_items.empty());
  CHECK(sel->from.table == "users");
  CHECK(!sel->from.alias.has_value());
  CHECK(sel->joins.empty());
  CHECK(sel->where == nullptr);
}

void TestSelectColumnListWithAliasesAndQualifiers() {
  auto stmt = ParseSQL("SELECT u.id AS uid, name FROM users AS u");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->is_star == false);
  CHECK(sel->select_items.size() == 2);

  CHECK(sel->select_items[0].table.has_value());
  CHECK(*sel->select_items[0].table == "u");
  CHECK(sel->select_items[0].column == "id");
  CHECK(sel->select_items[0].alias.has_value());
  CHECK(*sel->select_items[0].alias == "uid");

  CHECK(!sel->select_items[1].table.has_value());
  CHECK(sel->select_items[1].column == "name");
  CHECK(!sel->select_items[1].alias.has_value());

  CHECK(sel->from.table == "users");
  CHECK(sel->from.alias.has_value() && *sel->from.alias == "u");
}

void TestSelectWithJoin() {
  auto stmt = ParseSQL(
      "SELECT * FROM orders JOIN users ON orders.user_id = users.id");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->from.table == "orders");
  CHECK(sel->joins.size() == 1);
  CHECK(sel->joins[0].table.table == "users");
  CHECK(sel->joins[0].on_condition != nullptr);
  CHECK(sel->joins[0].on_condition->kind == ExprKind::kComparison);

  auto* cmp = static_cast<ComparisonExpr*>(sel->joins[0].on_condition.get());
  CHECK(cmp->op == ComparisonOp::kEq);
  CHECK(cmp->left->kind == ExprKind::kColumnRef);
  auto* left = static_cast<ColumnRefExpr*>(cmp->left.get());
  CHECK(left->table.has_value() && *left->table == "orders");
  CHECK(left->column == "user_id");
}

void TestSelectWithMultipleJoins() {
  auto stmt = ParseSQL(
      "SELECT * FROM a JOIN b ON a.id = b.a_id JOIN c ON b.id = c.b_id");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->joins.size() == 2);
  CHECK(sel->joins[0].table.table == "b");
  CHECK(sel->joins[1].table.table == "c");
}

void TestSelectJoinRequiresOn() {
  ExpectParseError("SELECT * FROM a JOIN b");
}

// Verifies the WHERE clause parses with the right operator PRECEDENCE:
// "a = 1 OR b = 2 AND c = 3" must parse as "a=1 OR (b=2 AND c=3)"
void TestWhereOperatorPrecedence() {
  auto stmt = ParseSQL("SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->where != nullptr);
  CHECK(sel->where->kind == ExprKind::kOr);

  auto* or_expr = static_cast<OrExpr*>(sel->where.get());
  CHECK(or_expr->left->kind == ExprKind::kComparison);
  CHECK(or_expr->right->kind == ExprKind::kAnd);

  auto* and_expr = static_cast<AndExpr*>(or_expr->right.get());
  CHECK(and_expr->left->kind == ExprKind::kComparison);
  CHECK(and_expr->right->kind == ExprKind::kComparison);
}

void TestWhereParenthesesOverridePrecedence() {
  // "(a = 1 OR b = 2) AND c = 3" — parens force the OR to bind first,
  // so the root must be an AndExpr this time, with an OrExpr on its left.
  auto stmt = ParseSQL("SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  CHECK(sel->where->kind == ExprKind::kAnd);
  auto* and_expr = static_cast<AndExpr*>(sel->where.get());
  CHECK(and_expr->left->kind == ExprKind::kOr);
  CHECK(and_expr->right->kind == ExprKind::kComparison);
}

void TestWhereAllComparisonOperators() {
  std::vector<std::pair<std::string, ComparisonOp>> cases = {
      {"=", ComparisonOp::kEq},   {"!=", ComparisonOp::kNeq},
      {"<", ComparisonOp::kLt},   {"<=", ComparisonOp::kLte},
      {">", ComparisonOp::kGt},   {">=", ComparisonOp::kGte},
  };
  for (auto& [op_text, op] : cases) {
    auto stmt = ParseSQL("SELECT * FROM t WHERE a " + op_text + " 1");
    auto* sel = static_cast<SelectStmt*>(stmt.get());
    auto* cmp = static_cast<ComparisonExpr*>(sel->where.get());
    CHECK(cmp->op == op);
  }
}

void TestWhereLiteralOnLeftSide() {
  auto stmt = ParseSQL("SELECT * FROM t WHERE 1 = a");
  auto* sel = static_cast<SelectStmt*>(stmt.get());
  auto* cmp = static_cast<ComparisonExpr*>(sel->where.get());
  CHECK(cmp->left->kind == ExprKind::kLiteral);
  CHECK(cmp->right->kind == ExprKind::kColumnRef);
}

void TestTrailingSemicolonIsOptional() {
  ParseSQL("SELECT * FROM t");   // no throw
  ParseSQL("SELECT * FROM t;");  // no throw
}

void TestMultipleStatementsRejected() {
  ExpectParseError("SELECT * FROM t; SELECT * FROM u");
}

void TestUnsupportedConstructsRejected() {
  ExpectParseError("UPDATE t SET a = 1");
  ExpectParseError("DELETE FROM t");
  ExpectParseError("SELECT COUNT(*) FROM t");
  ExpectParseError("SELECT * FROM t WHERE NOT a = 1");
  ExpectParseError("SELECT * FROM t LEFT JOIN u ON t.id = u.id");
  ExpectParseError("SELECT * FROM t WHERE a IN (1, 2)");
}

void TestGarbageInputRejected() {
  ExpectParseError("");
  ExpectParseError("this is not sql");
  ExpectParseError("SELECT");
  ExpectParseError("CREATE TABLE");
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"CreateTableBasic", TestCreateTableBasic},
      {"CreateTableRequiresAtLeastOneColumn", TestCreateTableRequiresAtLeastOneColumn},
      {"CreateTableRejectsUnknownType", TestCreateTableRejectsUnknownType},
      {"InsertWithExplicitColumns", TestInsertWithExplicitColumns},
      {"InsertWithoutColumnList", TestInsertWithoutColumnList},
      {"InsertRejectsMismatchedParens", TestInsertRejectsMismatchedParens},
      {"SelectStar", TestSelectStar},
      {"SelectColumnListWithAliasesAndQualifiers", TestSelectColumnListWithAliasesAndQualifiers},
      {"SelectWithJoin", TestSelectWithJoin},
      {"SelectWithMultipleJoins", TestSelectWithMultipleJoins},
      {"SelectJoinRequiresOn", TestSelectJoinRequiresOn},
      {"WhereOperatorPrecedence", TestWhereOperatorPrecedence},
      {"WhereParenthesesOverridePrecedence", TestWhereParenthesesOverridePrecedence},
      {"WhereAllComparisonOperators", TestWhereAllComparisonOperators},
      {"WhereLiteralOnLeftSide", TestWhereLiteralOnLeftSide},
      {"TrailingSemicolonIsOptional", TestTrailingSemicolonIsOptional},
      {"MultipleStatementsRejected", TestMultipleStatementsRejected},
      {"UnsupportedConstructsRejected", TestUnsupportedConstructsRejected},
      {"GarbageInputRejected", TestGarbageInputRejected},
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