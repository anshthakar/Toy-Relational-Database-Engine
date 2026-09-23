# Toy Relational Database Engine

A relational database engine built from scratch in C++, for learning
storage internals, crash recovery, and query planning. Not production
software, correctness and clear design are prioritized over feature
breadth.

## Status

- Milestone 1 — Storage engine: on-disk B+tree, buffer pool with
  LRU eviction, tested against `std::map` under randomized
  insert/delete/get/range-scan sequences.
- Milestone 2 — Write-ahead log and crash recovery: WAL with
  full-page-image records, REDO-only recovery, tested by both
  deterministic WAL truncation (~250 truncation points) and real
  `fork`+`SIGKILL` process death.
- Milestone 3 — SQL parser**
- Milestone 4 — Query planner and execution

## Architecture

```
include/
├── page.h            Page, PageHeader — the fixed 4096-byte on-disk unit
├── checksum.h         FNV-1a checksum, detects torn/corrupted page writes
├── disk_manager.h      Raw file I/O: read/write pages at fixed offsets
├── buffer_pool.h       In-memory page cache: LRU eviction, PageGuard (RAII pins)
├── btree_node.h        B+tree cell layout (LeafCell, InternalCell, NodeExtra)
├── btree.h             BTree: insert / delete / get / range scan
├── wal.h               WALManager: fixed-size, full-page-image WAL records
├── recovery.h           RunRecovery: replay the WAL onto the data file
└── database.h           Database: wires disk + WAL + recovery + buffer pool + tree together

src/
├── disk_manager.cpp
├── buffer_pool.cpp
├── btree.cpp
├── wal.cpp
├── recovery.cpp
└── database.cpp

tests/
├── test_disk_manager.cpp   Round-trip I/O, corruption detection, persistence
├── test_buffer_pool.cpp    Pin/unpin correctness, LRU order, dirty-page flush
├── test_btree.cpp          Differential fuzz test vs. std::map, multi-level splits
├── test_wal.cpp            Append/reopen, torn-tail truncation on open
├── test_recovery.cpp       ~250 deterministic WAL-truncation crash simulations
├── crash_worker.cpp        Standalone binary: inserts N keys, then raise(SIGKILL)
└── test_crash_kill.cpp     Forks/execs crash_worker, confirms real SIGKILL death,
                             verifies recovery in a separate process
```

Each layer only talks to the one below it: `BTree` → `BufferPool` →
(`DiskManager`, `WALManager`) → the file.
`Database` is the entry point that gets construction order right:
open data file → open WAL (self-truncates any torn tail) → run recovery
→ only then start a WAL-backed buffer pool.

## Design decisions

- **B+tree over LSM-tree.** In-place mutation gives a much more direct path
  to understanding page-level WAL and recovery (Milestone 2) than an
  LSM's compaction-based durability story.
- **Fixed-size values (32 bytes), not variable-length.** Trades value
  flexibility for a much simpler node format, no free-space tracking or
  slot indirection needed for the B-tree specifically.
- **No rebalancing on delete.** Deletes remove the key and shrink the
  node; no merge/borrow from siblings. Nodes can end up sparser than a
  production B-tree would allow, but the structure stays correct.
- **Artificially small fanout** (4 keys/node) so a few hundred test
  insertions exercise leaf splits, internal splits, and multi-level
  growth, real 4KB pages could hold far more per node.
- **LRU eviction**, plain, no scan-resistance (LRU-K, clock).
- **WAL logs full page images, not byte-level diffs**, fsyncing on every
  append (no group commit), REDO-only (no UNDO — there's no transaction
  concept yet to roll back). See `docs/milestone-2.md` for the
  write-ahead cross-page ordering invariant this depends on, and the
  real bug that violating it caused.

Full writeups: `docs/milestone-1.md`, `docs/milestone-2.md`.

## Building

Requires a POSIX environment, `DiskManager` uses `open`/`pread`/`pwrite`/
`fsync` directly, so **Linux or WSL2**, not native Windows/MSVC/MinGW.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

Debug builds compile with AddressSanitizer and UndefinedBehaviorSanitizer
on by default (see `CMakeLists.txt`), this is deliberate: the project is
judged on correctness under crashes, so memory bugs and undefined
behavior need to be hard failures, not silent.

## Running tests

```bash
cd build
./tests/test_disk_manager
./tests/test_buffer_pool
./tests/test_btree
./tests/test_wal
./tests/test_recovery      # ~9s — ~250 WAL-truncation crash simulations
./tests/test_crash_kill    # forks real processes and SIGKILLs them
```

Or via CTest:

```bash
ctest --test-dir build --output-on-failure
```

## Requirements

- CMake 3.16+
- A C++20 compiler (developed against GCC 15 via WSL2 Ubuntu)