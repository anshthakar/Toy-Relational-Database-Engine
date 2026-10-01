#pragma once

#include <stdexcept>
#include <string>

#include "ast.h"
#include "exec_row.h"
#include "value.h"

namespace reldb {

// Thrown for anything that makes an expression unevaluable against a
// given row: comparing values of different kinds (INTEGER vs. TEXT — this
// project doesn't implicitly convert between them), or handing a
// boolean-shaped node (AndExpr/OrExpr/ComparisonExpr) to EvalValue, or a
// value-shaped node (ColumnRefExpr/LiteralExpr) to EvalBool. The parser
// never actually constructs a malformed tree like that (see ast.h), so
// this really only fires if a logical-plan builder somewhere passes the
// wrong subtree — a programming error, not a user-facing SQL error, but
// still better to throw a clear message than read garbage.
struct EvalError : std::runtime_error {
  explicit EvalError(const std::string& msg) : std::runtime_error(msg) {}
};

// Converts a parsed SQL literal (ast.h's Literal — text as the parser saw
// it) into a runtime Value. Exported (not just an EvalValue implementation
// detail) because Engine's INSERT handling needs the exact same
// conversion for VALUES literals, which never go through a ColumnRefExpr
// at all.
Value LiteralToValue(const Literal& lit);

// Evaluates a value-producing expression (ColumnRefExpr or LiteralExpr —
// the two leaf kinds a ComparisonExpr's left/right ever hold, per ast.h)
// against one row.
Value EvalValue(const Expr& expr, const ExecRow& row);

// Evaluates a boolean-producing expression (ComparisonExpr, AndExpr, or
// OrExpr — a WHERE/ON clause's root and every node below the top
// comparison, per the grammar in docs/milestone-3.md) against one row.
bool EvalBool(const Expr& expr, const ExecRow& row);

}  // namespace reldb
