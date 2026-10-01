#include "expr_eval.h"

#include "ast.h"

namespace reldb {

Value LiteralToValue(const Literal& lit) {
  return lit.kind == LiteralKind::kInteger ? Value::Int(lit.int_value)
                                            : Value::Text(lit.text_value);
}

namespace {

// -1 / 0 / 1, the usual three-way comparison — kept to one place so every
// ComparisonOp (kEq..kGte) is just a check against this result, rather
// than six near-duplicate branches each re-deriving ordering.
int CompareValues(const Value& a, const Value& b) {
  if (a.kind != b.kind) {
    throw EvalError(
        "cannot compare an INTEGER value with a TEXT value — this "
        "engine doesn't implicitly convert between column types");
  }
  if (a.kind == ValueKind::kInteger) {
    if (a.int_value < b.int_value) return -1;
    if (a.int_value > b.int_value) return 1;
    return 0;
  }
  if (a.text_value < b.text_value) return -1;
  if (a.text_value > b.text_value) return 1;
  return 0;
}

}  // namespace

Value EvalValue(const Expr& expr, const ExecRow& row) {
  switch (expr.kind) {
    case ExprKind::kColumnRef: {
      const auto& ref = static_cast<const ColumnRefExpr&>(expr);
      return row.Find(ref.table, ref.column);
    }
    case ExprKind::kLiteral: {
      const auto& lit = static_cast<const LiteralExpr&>(expr);
      return LiteralToValue(lit.value);
    }
    case ExprKind::kComparison:
    case ExprKind::kAnd:
    case ExprKind::kOr:
      throw EvalError(
          "EvalValue: expected a value expression (column reference or "
          "literal), got a boolean expression");
  }
  throw EvalError("EvalValue: unreachable — unknown ExprKind");
}

bool EvalBool(const Expr& expr, const ExecRow& row) {
  switch (expr.kind) {
    case ExprKind::kComparison: {
      const auto& cmp = static_cast<const ComparisonExpr&>(expr);
      Value left = EvalValue(*cmp.left, row);
      Value right = EvalValue(*cmp.right, row);
      int c = CompareValues(left, right);
      switch (cmp.op) {
        case ComparisonOp::kEq:
          return c == 0;
        case ComparisonOp::kNeq:
          return c != 0;
        case ComparisonOp::kLt:
          return c < 0;
        case ComparisonOp::kLte:
          return c <= 0;
        case ComparisonOp::kGt:
          return c > 0;
        case ComparisonOp::kGte:
          return c >= 0;
      }
      throw EvalError("EvalBool: unreachable — unknown ComparisonOp");
    }
    case ExprKind::kAnd: {
      const auto& a = static_cast<const AndExpr&>(expr);
      return EvalBool(*a.left, row) && EvalBool(*a.right, row);
    }
    case ExprKind::kOr: {
      const auto& o = static_cast<const OrExpr&>(expr);
      return EvalBool(*o.left, row) || EvalBool(*o.right, row);
    }
    case ExprKind::kColumnRef:
    case ExprKind::kLiteral:
      throw EvalError(
          "EvalBool: expected a boolean expression (comparison, AND, or "
          "OR), got a bare value expression");
  }
  throw EvalError("EvalBool: unreachable — unknown ExprKind");
}

}  // namespace reldb
