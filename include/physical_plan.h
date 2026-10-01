#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "catalog.h"
#include "exec_row.h"
#include "logical_plan.h"

namespace reldb {

// Physical query plan: HOW to actually compute what the logical plan
// asked for. A LogicalJoin says only "these two inputs, joined on this
// condition"; a PhysicalPlan node says which algorithm runs it
// (nested-loop or hash join) and, for multi-table queries, in what order
// the joins happen — both decided by EstimateCost below, not copied
// mechanically from the logical plan's (textual, left-to-right) shape.
//
// Execution model: pull-based iterators, same shape as the BTree's own
// RangeScan — Open() resets iteration state, Next() returns one row at a
// time (std::nullopt at end), Close() is a no-op placeholder for a real
// engine's resource cleanup (nothing here holds anything Next()/the
// destructor doesn't already release, but the method exists so adding
// that later — e.g. an index scan holding a cursor — doesn't change the
// interface).
class PhysicalPlan {
 public:
  virtual ~PhysicalPlan() = default;
  virtual void Open() = 0;
  virtual std::optional<ExecRow> Next() = 0;
  virtual void Close() = 0;

  // This node's own estimated output size, in rows — used by a PARENT
  // join node deciding its own strategy/side, and (for the top-level
  // join-order search) by BuildPhysicalPlan itself. A scan reports the
  // catalog's row count; a join reports left*right as a crude upper
  // bound (no selectivity estimation — a documented simplification, see
  // docs/milestone-4.md); a filter passes its input's estimate through
  // unchanged (same reasoning: no selectivity estimate for WHERE either).
  virtual uint64_t EstimatedRows() const = 0;
};

// Leaf: reads every row of one table via Catalog::ScanTable, tagging each
// value with (alias, column name) so later Filter/Join/Project nodes can
// resolve column references through ExecRow::Find.
class PhysicalScan : public PhysicalPlan {
 public:
  PhysicalScan(Catalog* catalog, std::string table, std::string alias);
  void Open() override;
  std::optional<ExecRow> Next() override;
  void Close() override;
  uint64_t EstimatedRows() const override;

 private:
  Catalog* catalog_;
  std::string table_;
  std::string alias_;
  std::vector<std::vector<Value>> rows_;  // materialized on Open()
  size_t cursor_ = 0;
};

class PhysicalFilter : public PhysicalPlan {
 public:
  PhysicalFilter(std::unique_ptr<PhysicalPlan> input, const Expr* predicate);
  void Open() override;
  std::optional<ExecRow> Next() override;
  void Close() override;
  uint64_t EstimatedRows() const override;

  // Introspection only (tests, EXPLAIN-style debugging) — not used by
  // execution itself, which only ever calls the PhysicalPlan interface.
  const PhysicalPlan* input() const { return input_.get(); }

 private:
  std::unique_ptr<PhysicalPlan> input_;
  const Expr* predicate_;
};

// O(left * right): every left row tested against every right row. No
// requirement that `condition` be an equi-join — this is the fallback
// strategy that works for any ON condition the grammar allows (including
// <, <=, >, >=), which hash join cannot handle at all (see PhysicalHashJoin).
class PhysicalNestedLoopJoin : public PhysicalPlan {
 public:
  PhysicalNestedLoopJoin(std::unique_ptr<PhysicalPlan> left,
                         std::unique_ptr<PhysicalPlan> right,
                         const Expr* condition);
  void Open() override;
  std::optional<ExecRow> Next() override;
  void Close() override;
  uint64_t EstimatedRows() const override;

  const PhysicalPlan* left() const { return left_.get(); }
  const PhysicalPlan* right() const { return right_.get(); }

 private:
  std::unique_ptr<PhysicalPlan> left_;
  std::unique_ptr<PhysicalPlan> right_;
  const Expr* condition_;

  std::optional<ExecRow> current_left_;
  std::vector<ExecRow> right_rows_;  // materialized once, re-scanned per
                                      // left row (right_ itself only
                                      // supports one forward pass)
  size_t right_cursor_ = 0;
};

// O(left + right), but ONLY valid for a single top-level equality
// condition (col = col) — see ExtractEquiJoinColumns in physical_plan.cpp.
// Builds an in-memory hash table over the smaller side (`right`, by
// construction — see BuildPhysicalPlan's side-choice) keyed on the join
// column, then probes it once per row of `left`.
class PhysicalHashJoin : public PhysicalPlan {
 public:
  PhysicalHashJoin(std::unique_ptr<PhysicalPlan> build_side,
                    std::unique_ptr<PhysicalPlan> probe_side,
                    std::optional<std::string> build_table,
                    std::string build_column,
                    std::optional<std::string> probe_table,
                    std::string probe_column);
  void Open() override;
  std::optional<ExecRow> Next() override;
  void Close() override;
  uint64_t EstimatedRows() const override;

  const PhysicalPlan* build_side() const { return build_side_.get(); }
  const PhysicalPlan* probe_side() const { return probe_side_.get(); }

 private:
  std::unique_ptr<PhysicalPlan> build_side_;
  std::unique_ptr<PhysicalPlan> probe_side_;
  std::optional<std::string> build_table_;
  std::string build_column_;
  std::optional<std::string> probe_table_;
  std::string probe_column_;

  std::unordered_multimap<Value, ExecRow, ValueHash> build_table_rows_;
  // Equality on Value doesn't come with a transparent hash-bucket
  // equality check beyond operator== (already defined in value.h), so
  // std::unordered_multimap's default key_equal (std::equal_to<Value>)
  // works directly.
  std::vector<ExecRow> matches_for_current_probe_;
  size_t match_cursor_ = 0;
  std::optional<ExecRow> current_probe_;
};

class PhysicalProject : public PhysicalPlan {
 public:
  PhysicalProject(std::unique_ptr<PhysicalPlan> input, bool is_star,
                   std::vector<SelectItem> items);
  void Open() override;
  std::optional<ExecRow> Next() override;
  void Close() override;
  uint64_t EstimatedRows() const override;

  const PhysicalPlan* input() const { return input_.get(); }

 private:
  std::unique_ptr<PhysicalPlan> input_;
  bool is_star_;
  std::vector<SelectItem> items_;
};

// Translates a logical plan (see logical_plan.h — itself built from the
// AST) into a physical one: picks a join strategy per join (nested-loop
// or hash — see PhysicalNestedLoopJoin/PhysicalHashJoin above) and, for
// 3+ tables, a join order, both by estimated cost. The physical plan's
// shape does NOT have to mirror the logical plan's left-deep, textual-
// order tree — that's the whole point of having two separate stages: the
// logical plan canonicalizes what the query means, the physical planner
// is free to flatten it back into a table list and re-associate the
// joins however the cost model prefers (see docs/milestone-4.md for the
// exact cost model and the greedy join-ordering heuristic used here — not
// a full DP-optimal ordering, a documented simplification).
//
// `catalog` supplies EstimatedRowCount() for cost estimates and
// ScanTable() for execution. `logical_plan` must outlive the returned
// physical plan: PhysicalFilter/PhysicalNestedLoopJoin keep non-owning
// pointers into the same WHERE/ON expression trees the logical plan
// itself only pointed into (ultimately owned by the original SelectStmt).
std::unique_ptr<PhysicalPlan> BuildPhysicalPlan(const LogicalNode& logical_plan,
                                                 Catalog* catalog);

}  // namespace reldb
