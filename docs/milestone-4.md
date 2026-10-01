# Milestone 4 — Query planner and execution

Takes milestone 3's AST and actually runs it: a persistent multi-table
catalog, semantic validation, a logical plan, a cost-based physical plan
with two join strategies and basic join-order selection, and a pull-based
executor. All of it sits behind one new entry point, `Engine::ExecuteSQL`,
built in parallel to milestone 1/2's `Database` — `Database`/`BTree`'s
single-table behavior is untouched; `Engine` is the multi-table analogue
that milestone 4 needed and milestone 1/2 never did.

## The multi-table gap, and how it's closed

Milestones 1–3 never needed more than one table: `Database` owns exactly
one `BTree`, rooted at a private metadata page (`page_id` 0). Milestone 4
needs `CREATE TABLE` to register arbitrarily many tables, each with its
own schema and its own rows — but changing `BTree`'s or `Database`'s
existing shape risked breaking the single-table tests and designs
milestones 1/2 are graded on.

The fix is additive, not a rewrite:

- **`BTree` gained a second constructor** (`btree.h`/`btree.cpp`) that
  takes an explicit `root_page_id` and an `on_root_changed` callback,
  instead of owning a private metadata page. On a root split, it calls
  the callback instead of writing to a metadata page — everything else
  about `BTree` (insert, split, range scan, WAL interaction) is
  unchanged. The original single-argument constructor and all of
  `BTree`'s existing behavior are untouched; `Database` still uses it
  exactly as before.
- **`Catalog`** (`catalog.h`/`catalog.cpp`) is the new owner of "which
  tables exist, what's their schema, where's their data": a single fixed-
  size catalog page (always `page_id` 0 of a catalog-managed database
  file, by the same "allocate it first" convention `BTree`'s old metadata
  page used), listing up to `MAX_CATALOG_TABLES` (8) tables, each with up
  to `MAX_CATALOG_COLUMNS` (8) columns, a root page id, and a row count.
  Every table gets its own `BTree`, constructed via the new catalog-
  managed constructor, with `on_root_changed` wired to write that table's
  current root back into its catalog slot.
- Recovery needed **zero changes**. It replays raw page images by LSN; it
  has never cared what a page's bytes mean. A catalog page, a table's
  root leaf, and an internal node all recover exactly the same way they
  did in milestone 2 — multi-table support is "just" a new page layout
  plus bookkeeping about which `BTree` owns which pages, not a new
  recovery path.

### Cross-page write-ahead ordering, again

Milestone 2's central invariant — a page must be durably logged before
any page that references it — reappears twice in this milestone, and
both times it's handled the same way as `BTree::Insert`'s original root
split: confine the referenced page's `PageGuard` to its own nested scope
so it destructs (and WAL-logs) before the referencing page's `PageGuard`
is even constructed.

- `BTree`'s root-split code is now mode-aware: catalog-managed trees call
  `on_root_changed_` instead of writing a metadata page, but the ordering
  guarantee is identical — the new root page's guard already went out of
  scope, and was already logged, before the callback (which itself pins
  and writes the catalog page) ever runs.
- `Catalog::CreateTable` allocates a table's initial empty root leaf page
  in its own scope first, then opens the catalog page in a second scope
  to record that root's page id. A crash between the two must never leave
  a catalog entry pointing at a root page that was never itself written —
  exactly the failure mode milestone 2's design doc already covers for
  the single-table case.

### Row encoding

Milestone 1's `BTree` stores one fixed-size value per key (`VALUE_SIZE`
bytes, raised from 32 to 120 bytes for this milestone — see
`btree_node.h`). A multi-column row doesn't fit that model directly, so
`RowCodec` (`row_codec.h`/`row_codec.cpp`) packs every *non-primary-key*
column into one fixed-width blob: `INTEGER` columns take 8 bytes,
`TEXT` columns take `TEXT_COLUMN_CAPACITY` (16) bytes — a 1-byte length
prefix plus up to 15 content bytes, rejecting (not truncating) anything
longer. The primary key column itself is never encoded into the blob: a
row's key is already the `BTree` key it's stored under, so storing it
twice would be pure waste. `Catalog::CreateTable` checks
`RowCodec::EncodedSize()` against `VALUE_SIZE` up front, so a schema that
can never fit a row fails at `CREATE TABLE` time with a clear message,
not on the first `INSERT`.

**Primary keys must be `INTEGER`.** This project's `BTree` keys are
`int64_t` — there's no representation for a `TEXT` key — so
`Catalog::CreateTable` rejects a `TEXT` primary key outright rather than
let it fail confusingly at the first insert.

## Logical plan

`BuildLogicalPlan` (`logical_plan.h`/`logical_plan.cpp`) turns a
`SelectStmt`'s shape into a tree mechanically, with no schema lookups and
no reordering: `FROM` becomes a `Scan`, each `JOIN` wraps the plan so far
on the left in a `Join` whose right side is a fresh `Scan`, `WHERE` (if
present) wraps everything in a `Filter`, and the select list is a
`Project` on top. This mirrors the query exactly as written —
left-to-right, no cost consideration — which is deliberate: the logical
plan's job is to canonicalize *what* the query means, not decide *how* to
run it.

## Physical plan

`BuildPhysicalPlan` (`physical_plan.h`/`physical_plan.cpp`) takes the
logical plan and decides *how*. It does not have to preserve the logical
plan's left-deep shape — it unwinds the `Scan`/`Join` chain back into a
flat table list (`FlattenJoins`) and re-associates the joins however its
cost model prefers. This two-stage split (logical canonicalizes, physical
re-optimizes) is the actual point of having two separate plan
representations, not just an exercise in indirection.

### Cost model

Two strategies, chosen per join by comparing an estimated cost:

- **Nested-loop** (`PhysicalNestedLoopJoin`): cost ≈ `left_rows *
  right_rows`. Works for *any* `ON` condition the grammar allows —
  equality, `<`, `<=`, `>`, `>=`, AND/OR combinations — because it just
  evaluates the real condition (via `EvalBool`) against every pair.
- **Hash join** (`PhysicalHashJoin`): cost ≈ `left_rows + right_rows`,
  but only applicable when the condition is a single top-level `column =
  column` comparison (`ExtractEquiJoin`) — anything else (a range
  comparison, an AND of multiple comparisons, a column-to-literal
  comparison) can't be hashed and falls back to nested-loop regardless of
  size. The hash table is built on whichever side has the smaller
  estimated row count, then probed with the larger side.

`Catalog::EstimatedRowCount` supplies the sizes — an *exact* count in
this implementation (bumped durably on every `INSERT`), used the way a
real optimizer would use an approximate statistic. `PhysicalFilter` and
single-input nodes pass their input's row estimate straight through: this
project does not model `WHERE`'s selectivity (no "assume 10% of rows
match a comparison" heuristic), so a filtered join's cost estimate is
only ever an upper bound, not a tightened one. Documented simplification,
not an oversight — real selectivity estimation needs column statistics
(histograms, distinct-value counts) this toy catalog doesn't keep.

### Join-order selection (3+ tables)

A greedy heuristic, not a DP-optimal ordering (System R's dynamic
programming over subsets, which this project's scope doesn't call for):
starting from the base `FROM` table, repeatedly pick — among tables not
yet joined whose `ON` condition only references tables already joined (or
no table at all) — the one with the smallest estimated row count, and
join it in next. This at least avoids the worst case of joining a huge
table early just because it was written first in the query text, without
attempting to find a globally optimal order. If no remaining join
connects to what's joined so far (an unqualified or forward-referencing
condition), the planner falls back to the next table in textual order —
correctness never depends on this fallback, since every join still
evaluates its real condition; only plan quality does.

`test_planner.cpp`'s `TestCostEstimatorChoosesHashJoinForLargeTables` and
`TestCostEstimatorChoosesNestedLoopForTinyTables` demonstrate the
estimator picking each strategy in a scenario sized to make that strategy
actually cheaper — not just that both classes exist, but that the choice
between them responds to the numbers.

## Execution

Pull-based iterators (`PhysicalPlan::Open`/`Next`/`Close`), the same shape
as `BTree::RangeScan`'s own iteration: `PhysicalScan` materializes a
table's rows via `Catalog::ScanTable` (no secondary indexing — scans
always walk the primary key range); `PhysicalFilter` re-pulls until a row
passes `EvalBool`; `PhysicalNestedLoopJoin`/`PhysicalHashJoin` combine
rows from two inputs into one wider `ExecRow`; `PhysicalProject` keeps or
renames fields per the select list.

`ExecRow` (`exec_row.h`/`exec_row.cpp`) is the runtime row type —
`vector<Value>` isn't enough once a `JOIN` can put two tables' worth of
"id" columns in the same row, so every field remembers which table/alias
it came from. `ExecRow::Find` resolves a (possibly unqualified) column
reference the way SQL does: an unqualified name must match exactly one
field, or it's a `ColumnResolutionError`, not a silent first-match guess.

`expr_eval.h`/`expr_eval.cpp` evaluates the same `Expr` AST milestone 3
produced — `EvalValue` for column refs/literals, `EvalBool` for
comparisons/AND/OR — against one `ExecRow`. Comparing an `INTEGER` to a
`TEXT` value throws rather than silently coercing; this project has no
implicit type conversion.

## Semantic validation

`semantic.h`/`semantic.cpp`'s `ValidateStatement` runs between parsing and
planning: table existence, duplicate-alias detection, and that every
column reference (`WHERE`, `ON`, and the `SELECT` list) resolves to
exactly one known table/alias — unknown or ambiguous column references
are rejected here, with a specific message, rather than surfacing later
as a `ColumnResolutionError` from deep inside the executor. `INSERT`'s
column/value count matching is checked here too. `CREATE TABLE`'s own
constraints (primary key shape, column limits, row width) are
deliberately *not* duplicated here — `Catalog::CreateTable` is already
the one place that enforces them, and a second copy of the same checks
would just be two places that could drift apart.

## What's still NOT supported (carried over from milestone 3, plus new
## milestone-4-specific limitations)

- Everything milestone 3's grammar already excludes (`UPDATE`, `DELETE`,
  subqueries, `ORDER BY`/`GROUP BY`/`LIMIT`, aggregates, arithmetic,
  `NOT`/`IN`/`LIKE`/`BETWEEN`, outer joins) — there's no AST for any of
  it to plan or execute.
- A fixed catalog of at most 8 tables, 8 columns each, with fixed-width
  names (32 bytes/table, 24 bytes/column) and no `DROP TABLE` (catalog
  slots are never reclaimed).
- `TEXT` columns cap at 15 content bytes; a row's total non-key column
  width must fit in 120 bytes.
- Primary keys must be `INTEGER`.
- `INSERT` with an explicit column list must name every column exactly
  once — no partial inserts, since there's no column-default concept.
- No selectivity estimation for `WHERE`, no column statistics beyond
  per-table row counts, no secondary indexes (every scan is a full
  primary-key range scan).
- Join-order selection is a greedy heuristic, not a cost-optimal search.
- Not thread-safe, same as every prior milestone — concurrency is an
  explicitly out-of-scope stretch goal.

## Testing strategy

New test binaries, same hand-rolled `CHECK`-macro style as every other
milestone:

- **`test_row_codec.cpp`** — encode/decode round-trips, empty `TEXT`
  values, wrong value counts, type mismatches, and the exact-boundary
  `TEXT` length case.
- **`test_catalog.cpp`** — `CREATE TABLE` validation (duplicate names, no
  primary key, two primary keys, `TEXT` primary key, too many columns, a
  row too wide for `VALUE_SIZE`), multiple independent tables with enough
  rows to force real `BTree` splits in each, and a reopen test that
  destroys and reconstructs the `BufferPool`/`Catalog` over the same file
  to confirm schemas, root pointers, and row counts are all durable, not
  just in-memory bookkeeping.
- **`test_expr_eval.cpp`** — column resolution (qualified, unqualified,
  ambiguous-rejected), every comparison operator, AND/OR, and type-
  mismatch rejection.
- **`test_planner.cpp`** — the cost estimator demonstrably choosing each
  join strategy in a sized-for-that-outcome scenario, a non-equi condition
  always falling back to nested-loop regardless of table size, a 3-table
  join's correctness, `WHERE` filtering after a join, and projection with
  an alias.
- **`test_executor.cpp`** — end-to-end `Engine::ExecuteSQL` round trips
  (`CREATE TABLE` → `INSERT` → `SELECT`, including a join), semantic
  errors being rejected without mutating any table, `CREATE TABLE`
  constraint errors, and an engine-level reopen test — the milestone 4
  analogue of `test_recovery.cpp` — confirming a fresh `Engine` over the
  same files sees every previously inserted row and can still accept new
  writes.

No differential testing against a reference SQL engine, for the same
reason milestone 3 has none: there's no simpler reference implementation
of "this project's specific cost model and join-order heuristic" to
compare against. Hand-picked scenarios sized to make one strategy or
ordering clearly better are the direct substitute, the same approach
milestone 3 took for parser shape assertions.
