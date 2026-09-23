# Milestone 1 — Storage engine

## Scope

An on-disk B+tree with a fixed-page buffer pool underneath it. Insert,
delete, point lookup, range scan. Tested by fuzzing random operation
sequences against `std::map` as a reference implementation.

## B+tree over LSM-tree

The project's stated goal is understanding storage internals and crash
recovery, not raw write throughput. A B+tree mutates pages in place,
which gives a direct, traceable path from "a mutation happened" to "which
bytes on which page changed" — exactly the granularity Milestone 2's WAL
needs to reason about. An LSM-tree's durability story is mostly "replay
the active memtable's WAL, the rest is immutable SSTables," which is a
real mechanism but a much shallower one to learn from, and most of an
LSM's actual complexity budget (compaction strategy, leveling vs.
tiering, bloom filters) has nothing to do with recovery or query
planning at all. If the goal were write-throughput engineering, LSM would
win. It isn't, so B+tree does.

## Page format

Fixed 4096-byte pages (matches the common OS/filesystem block size).
Slotted-page layout: a small header, a slot array that would grow
downward from the header if this project used variable-length cells
(see the fixed-size-values note below — B-tree nodes here use a plain
array instead), and cell data.

`PageHeader` fields: `page_id`, `page_type`, `num_slots`,
`free_space_ptr`, `lsn`, `checksum`. The `lsn` field exists specifically
for Milestone 2 — it wasn't decoration, it's what lets recovery compare
"this WAL record's LSN" against "this page's last-applied LSN." The
`checksum` (FNV-1a, computed on write, verified on read) is what makes a
torn or corrupted page a **detected** failure instead of a silent one;
without it, Milestone 2's crash tests would have no way to distinguish
"correctly recovered" from "quietly wrong."

**A real bug this format caused, and how it was fixed:** the original
`PageHeader` was tightly packed (`#pragma pack(1)`) with no regard for
alignment. Once the B-tree's `int64_t` keys were laid out in packed cell
arrays sitting right after the header, most array elements ended up at
addresses that aren't 8-byte aligned — undefined behavior under the C++
object model, even though x86 hardware tolerates it silently. This is
exactly the kind of bug that would have shipped invisibly without
sanitizers: UBSan caught it immediately (`reference binding to
misaligned address ... requires 8 byte alignment`). Fix: `PageHeader` is
padded to 24 bytes (a multiple of 8), `Page` is declared `alignas(8)`,
and both `LeafCell` and `InternalCell` are padded so their array stride
is also a multiple of 8 — with `static_assert`s enforcing all three
constraints so a future edit can't silently reintroduce the bug.

## Buffer pool

Fixed-capacity frame array, a page table (`page_id -> frame_id`) for O(1)
lookup of cached pages, and an LRU list for eviction ordering.

**Eviction policy: plain LRU**, not LRU-K or clock. The project asked for
LRU explicitly, and it's the right pedagogical choice: you get "why does
eviction policy matter" without the added complexity of scan-resistance,
which is a real production concern but orthogonal to this project's
goals.

**RAII pinning (`PageGuard`)** is the actual mechanism — not a
convention — that prevents "forgot to unpin" bugs. A page is pinned for
as long as some `PageGuard` referencing it is alive; the guard's
destructor unpins automatically, on every code path (normal return,
early return, exception unwinding). There is no way to hold a pin
without a guard whose destruction will release it.

**Documented simplification:** `FindVictimFrame`'s scan for an unpinned
frame is O(n) over the whole pool in the worst case, not O(1). A real
buffer pool tracks pinned/unpinned frames in separate structures so
eviction cost doesn't depend on pool size. This project accepts O(n)
because pool sizes here are small (tens of frames in tests, not
millions), and the added bookkeeping wasn't worth it for what this
project is trying to teach. Worth being able to say precisely if asked.

## B+tree

Internal nodes hold only routing separator keys; all key/value data
lives in leaves, which are chained via a `next_leaf` pointer so range
scans don't need to re-descend from the root for every key.

**Documented simplification #1: fixed-size values (32 bytes), not the
variable-length slotted layout used for generic pages.** This trades
value flexibility for a much simpler node format — cells are a plain
sorted fixed-stride array, no free-space tracking or slot indirection
needed specifically for the B-tree. A production engine would want
variable-length values; this project judged that complexity as not
adding to the core learning goals (splits, recovery, query planning).

**Documented simplification #2: no rebalancing on delete.** Deletes
remove the key and shrink the node; there's no merge-with-sibling or
borrow-from-sibling logic. Nodes can end up sparser than a production
B-tree would tolerate, but the tree structure remains fully correct and
searchable. Real B-trees rebalance to bound worst-case space usage;
implementing that roughly doubles delete-path complexity for a benefit
(space efficiency) this project doesn't need to demonstrate. A clearly
stated stretch goal, not an oversight.

**Documented simplification #3: artificially small fanout** (`LEAF_MAX_KEYS
= INTERNAL_MAX_KEYS = 4`), far below what a real 4096-byte page could
hold. This is a standard testing technique, not a bug: it means a few
hundred insertions reliably force leaf splits, internal splits, and
multi-level tree growth, instead of needing tens of thousands of
insertions to see the same structural coverage.

## Testing strategy

Every component gets a differential/property test, not just
example-based unit tests:

- `test_disk_manager`: round-trip read/write, checksum catches a
  deliberately corrupted byte on disk, state survives a simulated
  process restart (close and reopen the file).
- `test_buffer_pool`: a pinned page is never evicted (the actual
  use-after-free bug this design exists to prevent), LRU evicts the
  correct frame under a forced-small pool, a dirty page's content
  survives eviction and a subsequent restart.
- `test_btree`: `BasicSmoke` for the obvious cases, `ForcesMultiLevelSplits`
  to confirm the tree actually grows past a single level (not just leaf
  splits), and `DifferentialFuzz` — 5000 randomized insert/delete/get
  operations checked against `std::map`, with periodic full and partial
  range-scan cross-checks, using a fixed RNG seed for reproducibility.

All tests run under AddressSanitizer and UndefinedBehaviorSanitizer in
Debug builds (see the top-level `CMakeLists.txt`). This isn't optional
for a project graded on correctness under crashes — a passing test suite
with silent undefined behavior underneath it isn't actually passing.