#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ast.h"

namespace reldb {

// Logical query plan: WHAT the query asks for, not HOW to compute it —
// no join algorithm, no join order decision yet (that's the physical
// plan's job, see physical_plan.h). A logical plan is a tree of these
// nodes, built directly and mechanically from a SelectStmt's AST shape:
//   SELECT ... FROM t [JOIN t2 ON c2] [JOIN t3 ON c3] ... [WHERE w]
// becomes, from the bottom up:
//   Scan(t) -> Join(ON c2) with Scan(t2) -> Join(ON c3) with Scan(t3)
//   -> Filter(w) [only if WHERE was present] -> Project(select list)
// i.e. every JOIN clause wraps the plan built so far on its LEFT side —
// this mirrors SQL's textual join order exactly (left-to-right, as
// written), which is deliberately naive: the physical planner is what's
// allowed to reorder joins for cost (milestone 4's join-order-selection
// requirement), not this layer.
enum class LogicalNodeKind { kScan, kFilter, kJoin, kProject };

struct LogicalNode {
  explicit LogicalNode(LogicalNodeKind k) : kind(k) {}
  virtual ~LogicalNode() = default;
  LogicalNodeKind kind;
};

// Reads every row of one table. `alias` is the name this table is known
// by for the rest of the plan (column qualifiers resolve against it) —
// equal to `table` when the query didn't write "AS".
struct LogicalScan : LogicalNode {
  LogicalScan() : LogicalNode(LogicalNodeKind::kScan) {}
  std::string table;
  std::string alias;
};

struct LogicalFilter : LogicalNode {
  LogicalFilter() : LogicalNode(LogicalNodeKind::kFilter) {}
  std::unique_ptr<LogicalNode> input;
  const Expr* predicate;  // non-owning: owned by the SelectStmt's AST,
                           // which the caller (BuildLogicalPlan) keeps
                           // alive for at least as long as this plan.
};

// Inner join only (per the grammar — see docs/milestone-3.md): every row
// of `left` paired with every row of `right` for which `condition` holds.
// Which side becomes the physical build/probe side, and in what order
// multiple joins execute, is the physical planner's decision — this node
// just records that a join belongs here and what its condition is.
struct LogicalJoin : LogicalNode {
  LogicalJoin() : LogicalNode(LogicalNodeKind::kJoin) {}
  std::unique_ptr<LogicalNode> left;
  std::unique_ptr<LogicalNode> right;
  const Expr* condition;  // non-owning, same lifetime note as above
};

// SELECT's output list. is_star mirrors SelectStmt::is_star directly —
// Project only has to decide which fields of an already-assembled ExecRow
// pass through, no fresh resolution logic (that's an EvalValue job).
struct LogicalProject : LogicalNode {
  LogicalProject() : LogicalNode(LogicalNodeKind::kProject) {}
  std::unique_ptr<LogicalNode> input;
  bool is_star = false;
  std::vector<SelectItem> items;  // empty when is_star
};

// Builds a logical plan from a parsed SELECT statement's shape alone — no
// schema lookups, no validation beyond what the grammar already
// guarantees (see docs/milestone-3.md's EBNF). Column/table existence and
// ambiguity are semantic checks the engine runs separately before
// building a physical plan (Engine::ExecuteSQL — task 15), not here.
//
// `stmt` must outlive the returned plan: LogicalFilter/LogicalJoin keep
// non-owning pointers into stmt's WHERE/ON expression trees rather than
// copying them.
std::unique_ptr<LogicalNode> BuildLogicalPlan(const SelectStmt& stmt);

}  // namespace reldb
