#include "logical_plan.h"

namespace reldb {

namespace {

std::unique_ptr<LogicalScan> MakeScan(const TableRef& ref) {
  auto scan = std::make_unique<LogicalScan>();
  scan->table = ref.table;
  scan->alias = ref.alias.has_value() ? *ref.alias : ref.table;
  return scan;
}

}  // namespace

std::unique_ptr<LogicalNode> BuildLogicalPlan(const SelectStmt& stmt) {
  // FROM — always exactly one base scan to start.
  std::unique_ptr<LogicalNode> plan = MakeScan(stmt.from);

  // Each JOIN clause wraps the plan so far on the left, matching the
  // left-to-right order the query was written in (see logical_plan.h's
  // class comment on why reordering is deliberately NOT done here).
  for (const JoinClause& join : stmt.joins) {
    auto node = std::make_unique<LogicalJoin>();
    node->left = std::move(plan);
    node->right = MakeScan(join.table);
    node->condition = join.on_condition.get();
    plan = std::move(node);
  }

  // WHERE, if present, filters everything assembled so far (scans +
  // joins) — SQL semantics: WHERE applies after the full FROM/JOIN cross
  // product the query describes, not per-table.
  if (stmt.where != nullptr) {
    auto filter = std::make_unique<LogicalFilter>();
    filter->input = std::move(plan);
    filter->predicate = stmt.where.get();
    plan = std::move(filter);
  }

  // SELECT list / star — always the outermost node, same as SQL's
  // logical evaluation order (FROM/JOIN, then WHERE, then SELECT).
  auto project = std::make_unique<LogicalProject>();
  project->input = std::move(plan);
  project->is_star = stmt.is_star;
  project->items = stmt.select_items;
  return project;
}

}  // namespace reldb
