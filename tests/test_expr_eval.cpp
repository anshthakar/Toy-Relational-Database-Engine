#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "ast.h"
#include "exec_row.h"
#include "expr_eval.h"
#include "value.h"

using namespace reldb;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      throw std::runtime_error(std::string("CHECK failed: ") + #cond +   \
                                " at " + __FILE__ + ":" +                \
                                std::to_string(__LINE__));                \
    }                                                                    \
  } while (0)

#define CHECK_THROWS(expr)                                               \
  do {                                                                   \
    bool threw = false;                                                  \
    try {                                                                \
      expr;                                                              \
    } catch (const std::exception&) {                                    \
      threw = true;                                                      \
    }                                                                    \
    CHECK(threw);                                                        \
  } while (0)

namespace {

ExecRow MakeOrderRow() {
  ExecRow row;
  row.fields.push_back({"orders", "id", Value::Int(5)});
  row.fields.push_back({"orders", "customer_id", Value::Int(2)});
  row.fields.push_back({"orders", "amount", Value::Int(100)});
  return row;
}

std::unique_ptr<ColumnRefExpr> Col(std::optional<std::string> table,
                                    std::string column) {
  auto e = std::make_unique<ColumnRefExpr>();
  e->table = std::move(table);
  e->column = std::move(column);
  return e;
}

std::unique_ptr<LiteralExpr> IntLit(int64_t v) {
  auto e = std::make_unique<LiteralExpr>();
  e->value.kind = LiteralKind::kInteger;
  e->value.int_value = v;
  return e;
}

std::unique_ptr<ComparisonExpr> Cmp(std::unique_ptr<Expr> l, ComparisonOp op,
                                     std::unique_ptr<Expr> r) {
  auto e = std::make_unique<ComparisonExpr>();
  e->op = op;
  e->left = std::move(l);
  e->right = std::move(r);
  return e;
}

void TestColumnRefResolution() {
  ExecRow row = MakeOrderRow();
  Value v = EvalValue(*Col("orders", "amount"), row);
  CHECK(v == Value::Int(100));

  // Unqualified works when unambiguous.
  Value v2 = EvalValue(*Col(std::nullopt, "customer_id"), row);
  CHECK(v2 == Value::Int(2));

  // Unknown column throws.
  CHECK_THROWS(EvalValue(*Col(std::nullopt, "nope"), row));
  CHECK_THROWS(EvalValue(*Col("orders", "nope"), row));
}

void TestAmbiguousUnqualifiedColumnRejected() {
  ExecRow row;
  row.fields.push_back({"a", "id", Value::Int(1)});
  row.fields.push_back({"b", "id", Value::Int(2)});
  CHECK_THROWS(EvalValue(*Col(std::nullopt, "id"), row));
  // Qualified still resolves fine.
  CHECK(EvalValue(*Col("b", "id"), row) == Value::Int(2));
}

void TestComparisonOperators() {
  ExecRow row = MakeOrderRow();
  CHECK(EvalBool(*Cmp(Col("orders", "amount"), ComparisonOp::kEq, IntLit(100)),
                 row));
  CHECK(!EvalBool(*Cmp(Col("orders", "amount"), ComparisonOp::kEq, IntLit(1)),
                  row));
  CHECK(EvalBool(*Cmp(Col("orders", "amount"), ComparisonOp::kGt, IntLit(50)),
                 row));
  CHECK(EvalBool(*Cmp(Col("orders", "amount"), ComparisonOp::kLte, IntLit(100)),
                 row));
  CHECK(EvalBool(*Cmp(Col("orders", "amount"), ComparisonOp::kNeq, IntLit(1)),
                 row));
}

void TestAndOr() {
  ExecRow row = MakeOrderRow();
  auto and_expr = std::make_unique<AndExpr>();
  and_expr->left = Cmp(Col("orders", "amount"), ComparisonOp::kGt, IntLit(50));
  and_expr->right = Cmp(Col("orders", "id"), ComparisonOp::kEq, IntLit(5));
  CHECK(EvalBool(*and_expr, row));

  auto or_expr = std::make_unique<OrExpr>();
  or_expr->left = Cmp(Col("orders", "amount"), ComparisonOp::kGt, IntLit(9999));
  or_expr->right = Cmp(Col("orders", "id"), ComparisonOp::kEq, IntLit(5));
  CHECK(EvalBool(*or_expr, row));

  auto both_false = std::make_unique<AndExpr>();
  both_false->left =
      Cmp(Col("orders", "amount"), ComparisonOp::kGt, IntLit(9999));
  both_false->right = Cmp(Col("orders", "id"), ComparisonOp::kEq, IntLit(5));
  CHECK(!EvalBool(*both_false, row));
}

void TestTypeMismatchComparisonRejected() {
  ExecRow row = MakeOrderRow();
  auto text_lit = std::make_unique<LiteralExpr>();
  text_lit->value.kind = LiteralKind::kText;
  text_lit->value.text_value = "100";
  CHECK_THROWS(EvalBool(
      *Cmp(Col("orders", "amount"), ComparisonOp::kEq, std::move(text_lit)),
      row));
}

void TestWrongShapeToEvalFunctionsRejected() {
  ExecRow row = MakeOrderRow();
  // A boolean expression handed to EvalValue.
  CHECK_THROWS(
      EvalValue(*Cmp(Col("orders", "id"), ComparisonOp::kEq, IntLit(5)), row));
  // A bare value expression handed to EvalBool.
  CHECK_THROWS(EvalBool(*Col("orders", "id"), row));
}

}  // namespace

int main() {
  struct NamedTest {
    const char* name;
    void (*fn)();
  };
  NamedTest tests[] = {
      {"TestColumnRefResolution", TestColumnRefResolution},
      {"TestAmbiguousUnqualifiedColumnRejected",
       TestAmbiguousUnqualifiedColumnRejected},
      {"TestComparisonOperators", TestComparisonOperators},
      {"TestAndOr", TestAndOr},
      {"TestTypeMismatchComparisonRejected",
       TestTypeMismatchComparisonRejected},
      {"TestWrongShapeToEvalFunctionsRejected",
       TestWrongShapeToEvalFunctionsRejected},
  };

  int failures = 0;
  for (const auto& t : tests) {
    try {
      t.fn();
      std::printf("[PASS] %s\n", t.name);
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", t.name, e.what());
      failures++;
    }
  }
  return failures == 0 ? 0 : 1;
}
