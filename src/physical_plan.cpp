#include "physical_plan.h"

#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

#include "expr_eval.h"

namespace reldb {

// ---------- PhysicalScan ----------

PhysicalScan::PhysicalScan(Catalog* catalog, std::string table,
                           std::string alias)
    : catalog_(catalog), table_(std::move(table)), alias_(std::move(alias)) {}

void PhysicalScan::Open() {
  rows_ = catalog_->ScanTable(table_);
  cursor_ = 0;
}

std::optional<ExecRow> PhysicalScan::Next() {
  if (cursor_ >= rows_.size()) return std::nullopt;
  const TableSchema& schema = catalog_->GetSchema(table_);
  ExecRow row;
  const std::vector<Value>& values = rows_[cursor_];
  row.fields.reserve(schema.columns.size());
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    row.fields.push_back({alias_, schema.columns[i].name, values[i]});
  }
  cursor_++;
  return row;
}

void PhysicalScan::Close() {
  rows_.clear();
  cursor_ = 0;
}

uint64_t PhysicalScan::EstimatedRows() const {
  return catalog_->EstimatedRowCount(table_);
}

// ---------- PhysicalFilter ----------

PhysicalFilter::PhysicalFilter(std::unique_ptr<PhysicalPlan> input,
                               const Expr* predicate)
    : input_(std::move(input)), predicate_(predicate) {}

void PhysicalFilter::Open() { input_->Open(); }

std::optional<ExecRow> PhysicalFilter::Next() {
  while (true) {
    std::optional<ExecRow> row = input_->Next();
    if (!row.has_value()) return std::nullopt;
    if (EvalBool(*predicate_, *row)) return row;
  }
}

void PhysicalFilter::Close() { input_->Close(); }

uint64_t PhysicalFilter::EstimatedRows() const {
  // No selectivity estimation — WHERE's filtering effect isn't modeled,
  // just passed through as an upper bound. A documented simplification
  // (see docs/milestone-4.md): a real optimizer would estimate a
  // selectivity fraction per predicate.
  return input_->EstimatedRows();
}

// ---------- PhysicalNestedLoopJoin ----------

PhysicalNestedLoopJoin::PhysicalNestedLoopJoin(
    std::unique_ptr<PhysicalPlan> left, std::unique_ptr<PhysicalPlan> right,
    const Expr* condition)
    : left_(std::move(left)), right_(std::move(right)), condition_(condition) {}

void PhysicalNestedLoopJoin::Open() {
  left_->Open();
  right_->Open();
  right_rows_.clear();
  std::optional<ExecRow> r;
  while ((r = right_->Next()).has_value()) {
    right_rows_.push_back(std::move(*r));
  }
  right_->Close();
  current_left_ = left_->Next();
  right_cursor_ = 0;
}

std::optional<ExecRow> PhysicalNestedLoopJoin::Next() {
  while (current_left_.has_value()) {
    while (right_cursor_ < right_rows_.size()) {
      ExecRow candidate = *current_left_;
      const ExecRow& r = right_rows_[right_cursor_];
      candidate.fields.insert(candidate.fields.end(), r.fields.begin(),
                               r.fields.end());
      right_cursor_++;
      if (EvalBool(*condition_, candidate)) {
        return candidate;
      }
    }
    current_left_ = left_->Next();
    right_cursor_ = 0;
  }
  return std::nullopt;
}

void PhysicalNestedLoopJoin::Close() { left_->Close(); }

uint64_t PhysicalNestedLoopJoin::EstimatedRows() const {
  return left_->EstimatedRows() * right_->EstimatedRows();
}

// ---------- PhysicalHashJoin ----------

PhysicalHashJoin::PhysicalHashJoin(std::unique_ptr<PhysicalPlan> build_side,
                                   std::unique_ptr<PhysicalPlan> probe_side,
                                   std::optional<std::string> build_table,
                                   std::string build_column,
                                   std::optional<std::string> probe_table,
                                   std::string probe_column)
    : build_side_(std::move(build_side)),
      probe_side_(std::move(probe_side)),
      build_table_(std::move(build_table)),
      build_column_(std::move(build_column)),
      probe_table_(std::move(probe_table)),
      probe_column_(std::move(probe_column)) {}

void PhysicalHashJoin::Open() {
  build_side_->Open();
  build_table_rows_.clear();
  std::optional<ExecRow> r;
  while ((r = build_side_->Next()).has_value()) {
    Value key = r->Find(build_table_, build_column_);
    build_table_rows_.emplace(std::move(key), std::move(*r));
  }
  build_side_->Close();

  probe_side_->Open();
  current_probe_ = probe_side_->Next();
  matches_for_current_probe_.clear();
  match_cursor_ = 0;
}

std::optional<ExecRow> PhysicalHashJoin::Next() {
  while (current_probe_.has_value()) {
    // Lazily compute this probe row's matching build-side rows the first
    // time it's looked at (match_cursor_ == 0 and nothing cached yet).
    // Re-checking this on every loop pass is cheap and avoids needing a
    // separate "have I computed matches for this row" flag.
    if (match_cursor_ == 0 && matches_for_current_probe_.empty()) {
      Value key = current_probe_->Find(probe_table_, probe_column_);
      auto range = build_table_rows_.equal_range(key);
      for (auto it = range.first; it != range.second; ++it) {
        matches_for_current_probe_.push_back(it->second);
      }
    }

    if (match_cursor_ < matches_for_current_probe_.size()) {
      ExecRow combined = *current_probe_;
      const ExecRow& build_row = matches_for_current_probe_[match_cursor_];
      combined.fields.insert(combined.fields.end(), build_row.fields.begin(),
                              build_row.fields.end());
      match_cursor_++;
      return combined;
    }

    // Exhausted (possibly zero) matches for this probe row — inner join,
    // so a probe row with no match simply contributes nothing.
    current_probe_ = probe_side_->Next();
    matches_for_current_probe_.clear();
    match_cursor_ = 0;
  }
  return std::nullopt;
}

void PhysicalHashJoin::Close() { probe_side_->Close(); }

uint64_t PhysicalHashJoin::EstimatedRows() const {
  return build_side_->EstimatedRows() + probe_side_->EstimatedRows();
}

// ---------- PhysicalProject ----------

PhysicalProject::PhysicalProject(std::unique_ptr<PhysicalPlan> input,
                                 bool is_star, std::vector<SelectItem> items)
    : input_(std::move(input)), is_star_(is_star), items_(std::move(items)) {}

void PhysicalProject::Open() { input_->Open(); }

std::optional<ExecRow> PhysicalProject::Next() {
  std::optional<ExecRow> row = input_->Next();
  if (!row.has_value()) return std::nullopt;
  if (is_star_) return row;

  ExecRow projected;
  projected.fields.reserve(items_.size());
  for (const SelectItem& item : items_) {
    const Value& v = row->Find(item.table, item.column);
    // Output field naming: this is the FINAL row the caller sees, so
    // qualification no longer matters for further column resolution —
    // the output column name is the alias if one was given, else the
    // bare column name, and `table` is left as whatever was written
    // (possibly empty, unlike every other ExecRow in this pipeline,
    // which always carries a real table/alias — see exec_row.h).
    std::string out_table = item.table.has_value() ? *item.table : "";
    std::string out_column = item.alias.has_value() ? *item.alias : item.column;
    projected.fields.push_back({out_table, out_column, v});
  }
  return projected;
}

void PhysicalProject::Close() { input_->Close(); }

uint64_t PhysicalProject::EstimatedRows() const {
  return input_->EstimatedRows();
}

// ---------- BuildPhysicalPlan ----------

namespace {

// One base table in the flattened join list, with the (possibly null,
// only for the very first table) ON condition that introduced it.
struct TableNode {
  std::string table;
  std::string alias;
  const Expr* join_condition;  // nullptr only for nodes[0]
};

void CollectReferencedTables(const Expr& expr, std::set<std::string>* out) {
  switch (expr.kind) {
    case ExprKind::kColumnRef: {
      const auto& ref = static_cast<const ColumnRefExpr&>(expr);
      if (ref.table.has_value()) out->insert(*ref.table);
      return;
    }
    case ExprKind::kComparison: {
      const auto& cmp = static_cast<const ComparisonExpr&>(expr);
      CollectReferencedTables(*cmp.left, out);
      CollectReferencedTables(*cmp.right, out);
      return;
    }
    case ExprKind::kAnd: {
      const auto& a = static_cast<const AndExpr&>(expr);
      CollectReferencedTables(*a.left, out);
      CollectReferencedTables(*a.right, out);
      return;
    }
    case ExprKind::kOr: {
      const auto& o = static_cast<const OrExpr&>(expr);
      CollectReferencedTables(*o.left, out);
      CollectReferencedTables(*o.right, out);
      return;
    }
    case ExprKind::kLiteral:
      return;
  }
}

struct EquiJoinColumns {
  const ColumnRefExpr* a;
  const ColumnRefExpr* b;
};

// Only a single top-level "column = column" comparison counts as an
// equi-join this planner can hash — anything else (a range comparison,
// an AND of several comparisons, a column-to-literal comparison) falls
// back to nested-loop, which handles any ON condition the grammar allows.
std::optional<EquiJoinColumns> ExtractEquiJoin(const Expr& condition) {
  if (condition.kind != ExprKind::kComparison) return std::nullopt;
  const auto& cmp = static_cast<const ComparisonExpr&>(condition);
  if (cmp.op != ComparisonOp::kEq) return std::nullopt;
  if (cmp.left->kind != ExprKind::kColumnRef ||
      cmp.right->kind != ExprKind::kColumnRef) {
    return std::nullopt;
  }
  return EquiJoinColumns{static_cast<const ColumnRefExpr*>(cmp.left.get()),
                          static_cast<const ColumnRefExpr*>(cmp.right.get())};
}

// Unwinds BuildLogicalPlan's left-deep Scan/Join tree back into a flat
// table list — the physical planner re-associates these into whatever
// join order/strategy its cost model prefers, rather than executing the
// logical tree's shape directly.
void FlattenJoins(const LogicalNode* node, std::vector<TableNode>* out) {
  if (node->kind == LogicalNodeKind::kScan) {
    const auto* scan = static_cast<const LogicalScan*>(node);
    out->push_back({scan->table, scan->alias, nullptr});
    return;
  }
  if (node->kind == LogicalNodeKind::kJoin) {
    const auto* join = static_cast<const LogicalJoin*>(node);
    FlattenJoins(join->left.get(), out);
    // BuildLogicalPlan only ever puts a fresh LogicalScan on a join's
    // right side (one JoinClause introduces exactly one table) — see
    // logical_plan.cpp — so this is always a Scan, never another Join.
    const auto* right_scan = static_cast<const LogicalScan*>(join->right.get());
    out->push_back({right_scan->table, right_scan->alias, join->condition});
    return;
  }
  throw std::logic_error(
      "FlattenJoins: expected only Scan/Join nodes below Filter/Project — "
      "logical plan shape drifted from what BuildLogicalPlan produces");
}

}  // namespace

std::unique_ptr<PhysicalPlan> BuildPhysicalPlan(const LogicalNode& logical_plan,
                                                 Catalog* catalog) {
  // BuildLogicalPlan always wraps everything in exactly one outermost
  // LogicalProject, with an optional LogicalFilter directly beneath it.
  if (logical_plan.kind != LogicalNodeKind::kProject) {
    throw std::logic_error(
        "BuildPhysicalPlan: expected a Project node at the plan root");
  }
  const auto& project = static_cast<const LogicalProject&>(logical_plan);

  const LogicalNode* join_tree_root = project.input.get();
  const Expr* predicate = nullptr;
  if (join_tree_root->kind == LogicalNodeKind::kFilter) {
    const auto& filter = static_cast<const LogicalFilter&>(*join_tree_root);
    predicate = filter.predicate;
    join_tree_root = filter.input.get();
  }

  std::vector<TableNode> nodes;
  FlattenJoins(join_tree_root, &nodes);

  // Start the accumulated plan with the base FROM table — always nodes[0].
  std::unique_ptr<PhysicalPlan> accumulated =
      std::make_unique<PhysicalScan>(catalog, nodes[0].table, nodes[0].alias);
  std::set<std::string> joined_aliases = {nodes[0].alias};
  std::vector<bool> consumed(nodes.size(), false);
  consumed[0] = true;
  size_t remaining = nodes.size() - 1;

  while (remaining > 0) {
    // Greedy join-order heuristic (documented simplification — see
    // docs/milestone-4.md; not a full DP-optimal ordering): among the
    // not-yet-joined tables whose ON condition only references tables
    // already in the accumulated plan (or no table at all), pick the one
    // with the smallest estimated row count to join in next. This at
    // least avoids the worst case of joining a huge table early just
    // because it happened to be written first.
    int best_idx = -1;
    uint64_t best_rows = 0;
    for (size_t i = 1; i < nodes.size(); ++i) {
      if (consumed[i]) continue;
      std::set<std::string> refs;
      CollectReferencedTables(*nodes[i].join_condition, &refs);
      refs.erase(nodes[i].alias);
      bool connected = true;
      for (const auto& r : refs) {
        if (!joined_aliases.count(r)) {
          connected = false;
          break;
        }
      }
      if (!connected) continue;
      uint64_t rows = catalog->EstimatedRowCount(nodes[i].table);
      if (best_idx < 0 || rows < best_rows) {
        best_idx = static_cast<int>(i);
        best_rows = rows;
      }
    }
    if (best_idx < 0) {
      // No remaining join's condition resolves against what's joined so
      // far — e.g. it only references tables introduced later. Rather
      // than guess, fall back to the next unconsumed table in the
      // original textual order; correctness doesn't depend on this
      // choice (every strategy below still evaluates the real
      // condition), only how good the resulting plan is.
      for (size_t i = 1; i < nodes.size(); ++i) {
        if (!consumed[i]) {
          best_idx = static_cast<int>(i);
          break;
        }
      }
    }

    const TableNode& next = nodes[static_cast<size_t>(best_idx)];
    auto right_scan =
        std::make_unique<PhysicalScan>(catalog, next.table, next.alias);
    uint64_t right_rows = catalog->EstimatedRowCount(next.table);
    uint64_t left_rows = accumulated->EstimatedRows();

    const Expr* condition = next.join_condition;
    std::optional<EquiJoinColumns> equi = ExtractEquiJoin(*condition);

    uint64_t nested_cost = left_rows * right_rows;
    uint64_t hash_cost = equi.has_value()
                              ? left_rows + right_rows
                              : std::numeric_limits<uint64_t>::max();

    bool used_hash_join = false;
    if (equi.has_value() && hash_cost <= nested_cost) {
      const ColumnRefExpr* next_ref = nullptr;
      const ColumnRefExpr* acc_ref = nullptr;
      if (equi->a->table.has_value() && *equi->a->table == next.alias) {
        next_ref = equi->a;
        acc_ref = equi->b;
      } else if (equi->b->table.has_value() && *equi->b->table == next.alias) {
        next_ref = equi->b;
        acc_ref = equi->a;
      }
      if (next_ref != nullptr) {
        // Build the hash table on whichever side is smaller.
        if (left_rows <= right_rows) {
          accumulated = std::make_unique<PhysicalHashJoin>(
              std::move(accumulated), std::move(right_scan), acc_ref->table,
              acc_ref->column, next_ref->table, next_ref->column);
        } else {
          accumulated = std::make_unique<PhysicalHashJoin>(
              std::move(right_scan), std::move(accumulated), next_ref->table,
              next_ref->column, acc_ref->table, acc_ref->column);
        }
        used_hash_join = true;
      }
      // next_ref == nullptr means the equi-join's columns don't clearly
      // identify which side is the new table (an unqualified or
      // oddly-aliased condition) — fall through to nested-loop below
      // rather than guess at build/probe sides.
    }
    if (!used_hash_join) {
      accumulated = std::make_unique<PhysicalNestedLoopJoin>(
          std::move(accumulated), std::move(right_scan), condition);
    }

    joined_aliases.insert(next.alias);
    consumed[static_cast<size_t>(best_idx)] = true;
    remaining--;
  }

  std::unique_ptr<PhysicalPlan> plan = std::move(accumulated);
  if (predicate != nullptr) {
    plan = std::make_unique<PhysicalFilter>(std::move(plan), predicate);
  }
  plan = std::make_unique<PhysicalProject>(std::move(plan), project.is_star,
                                           project.items);
  return plan;
}

}  // namespace reldb
