# Milestone 2 — Write-ahead log and crash recovery

The spec calls this milestone "the core of the project." It's also the
one where the design only gets proven correct by adversarial testing —
two bugs described below were found by tests, not by inspection, and
both are the kind that would have shipped silently in a less carefully
tested codebase.

## WAL record format

Fixed-size records: a small header (`magic`, `lsn`, `page_id`,
`checksum`) followed by the **full 4096-byte after-image** of the
mutated page.

```
struct WALRecordHeader {
  uint32_t magic;
  uint64_t lsn;
  page_id_t page_id;
  uint32_t checksum;   // over {magic, lsn, page_id, payload}
};
// followed by PAGE_SIZE bytes: the page's complete post-mutation content
```

**Documented simplification: full-page-image logging, not byte-level
diffs.** Real WALs log small deltas to stay compact. Logging the whole
page instead means recovery only ever needs to *overwrite*, never
*reconstruct from a sequence of diffs* — replaying a record is just "make
this page equal to these bytes," which is nearly impossible to get
subtly wrong. The cost is a much larger log (~4KB per mutated page
instead of a few bytes), which is the right trade for understanding
*why* WAL works and the wrong one for a real engine's throughput.

**Documented simplification: fsync on every append, no group commit.**
Every `AppendRecord` call blocks until its `fsync` returns — the caller
never gets control back until the mutation is durable. Real systems
batch fsyncs across concurrent transactions for throughput; there's no
concurrency in this project yet, so batching would add real complexity
(buffering, flush thresholds) for no measurable benefit at this stage.

**Documented simplification: REDO-only, no UNDO.** There's no
transaction/rollback concept yet, so nothing logged is ever
"uncommitted" — every record represents a mutation that's meant to
exist. If transactions are added later (an explicit stretch goal per the
spec), this format alone isn't sufficient; UNDO records would be needed
too.

## Where logging hooks in

Every B-tree mutation already flows through `PageGuard`'s destructor →
`BufferPool::UnpinPage(page_id, is_dirty)`. That's the natural log point:
the moment a page stops being actively mutated and becomes "at rest." If
`is_dirty` and a `WALManager` is attached, `UnpinPage` calls
`AppendRecord` — which stamps `page.header()->lsn` and fsyncs — before
decrementing the pin count. This directly uses the `lsn` field that was
added to `PageHeader` back in Milestone 1; it wasn't decoration, this is
what it was for.

`BufferPool`'s `WALManager*` is optional (defaults to `nullptr`), so
Milestone 1's tests, which predate crash recovery, keep working
unmodified — they simply construct a pool with no WAL attached, and
logging is a no-op.

## Bug #1: cross-page write-ahead ordering in the root-split path

The single most important correctness property a multi-page WAL design
needs is: **a page must never become durably-referenced before the page
it references is itself durable.** If page P's content includes "child
is page C," then C's WAL record must be written (and fsynced) before P's
is. Get this backwards, and a crash in the gap leaves a durable pointer
to a page that was never written — a dangling reference baked into the
log itself.

Tracing through the B-tree's split-propagation code confirms this holds
almost everywhere for free: a split's new right sibling is a `PageGuard`
local to `SplitLeafIfNeeded`/`SplitInternalIfNeeded`, and it's destructed
(and thus logged) *before* that function returns to its caller, which
only afterward marks the parent dirty with a pointer to that sibling.
Since C++ destroys locals when their own function returns, and the
caller's own guard isn't released until *its* function returns even
later, every level of the recursion naturally logs child-before-parent.

The one place this didn't hold: `BTree::Insert`'s epilogue, when the
*root itself* splits and a brand new root is created:

```cpp
PageGuard new_root_guard = buffer_pool_->NewPage();
// ... fill in new_root_guard's content ...
root_page_id_ = new_root_guard.page_id();

PageGuard meta_guard = buffer_pool_->FetchPage(metadata_page_id_);
GetMetadata(meta_guard.page())->root_page_id = root_page_id_;
meta_guard.MarkDirty();
// both guards still alive here — neither has logged yet
```

Both guards are local to `Insert()` and stay alive until the function
returns. C++ destructs locals in **reverse declaration order** — so
`meta_guard` (declared second) is destructed, and therefore logged,
*first*, and `new_root_guard` logs *second*. That's backwards: the
metadata page (which now points at the new root) would become durable
before the new root's own content does. A crash in that narrow window
would leave a WAL whose durable prefix points at a root page that was
never itself made durable.

This wasn't caught by inspection — it was caught by deliberately
reasoning through the ordering guarantee the WAL-truncation crash test
depends on (see below) before trusting that test's results. The fix
scopes `new_root_guard` to close before `meta_guard` is even
constructed:

```cpp
page_id_t new_root_id;
{
  PageGuard new_root_guard = buffer_pool_->NewPage();
  // ...
  new_root_id = new_root_guard.page_id();
}  // logged here — before metadata references it

root_page_id_ = new_root_id;

{
  PageGuard meta_guard = buffer_pool_->FetchPage(metadata_page_id_);
  GetMetadata(meta_guard.page())->root_page_id = root_page_id_;
  meta_guard.MarkDirty();
}  // logged here — safe, new root is already durable
```

**The provable invariant, once this fix is in place:** for any single
operation, WAL records are always appended in strict child-before-parent
order. Since a truncated WAL is always a byte-offset *prefix*, and
children always sit at lower offsets than the parents that reference
them, truncating at *any* point yields a "downward-closed" set of
recovered pages — if a page's record survived, everything it points to
also survived. Recovery from an arbitrarily truncated WAL can therefore
never produce a dangling reference. This is the precise claim the crash
tests below verify, not just assume.

## Recovery algorithm

1. `WALManager`'s constructor scans the WAL from byte 0 in fixed-stride
   records, stopping at the first one that fails (bad magic, checksum
   mismatch, or a short read from a truncated file) — that's the boundary
   of the durable prefix. It then truncates the file back to exactly that
   boundary. This matters beyond just tidiness: without truncating, a
   future append would land *after* the torn tail, and since any future
   recovery scan also stops at the first invalid record, that torn tail
   would permanently block anything appended after it from ever being
   seen again.
2. `RunRecovery` reduces the valid records to "latest record per
   `page_id`" (last-LSN-wins) and writes each one's full after-image
   directly to the data file via `DiskManager::WritePage`.
3. `DiskManager::RefreshNextPageIdAfterRecovery` re-derives the
   allocation counter from the file's new size — necessary because
   recovery's direct writes can extend the file past whatever page count
   `DiskManager`'s constructor saw (which predates recovery).
4. Only after all of this does `Database` construct a `BufferPool` and
   hand out pages — starting the buffer pool before recovery finishes
   would mean the first reads could see pre-recovery bytes.

## Bug #2: `ValidRecords()` didn't reflect records just appended

A smaller bug, caught by `test_wal.cpp`: `AppendRecord` fsynced and wrote
a new record correctly, but never added it to `valid_records_`, which was
populated only at construction time from the file as it existed at open.
Calling `ValidRecords()` right after an in-process append therefore
undercounted. Recovery itself was never affected (it only ever calls
`ValidRecords()` once, immediately after construction, before any
appends happen in that instance), but the API's documented contract
("records found valid") was misleading about what it actually returned.
Fixed by having `AppendRecord` push the newly-written record onto
`valid_records_` too, so the accessor always reflects everything durably
known so far, not just the state as of the last file open.

## Crash testing: two independent methods

The spec explicitly asks for both WAL truncation and real process kills,
because they test different things.

### Deterministic WAL-truncation (`test_recovery.cpp`)

Inserts keys `0..299` (forcing real leaf, internal, and root splits)
through a small (8-frame) buffer pool, recording the WAL's on-disk byte
size after every top-level `Insert()` call. For roughly 250 truncation
points — both exact operation boundaries and deliberately mid-record
("torn write") points — a copy of the WAL is truncated to that byte
offset and recovery is run against it, then the result is checked:

- At an exact boundary (`T = size_after[i]`), the recovered key set must
  be **exactly** `{0..i}` — not "roughly," exactly, with every value
  correct. Nothing looser is acceptable at a clean boundary.
- At a mid-record point, the recovered key set must be exactly `{0..i}`
  or `{0..i+1}` — the only two outcomes the write-ahead-ordering
  invariant permits — with every returned value correct and keys
  strictly sorted.

**A test-methodology bug worth documenting on its own:** the first
version of this test paired the truncated WAL with a copy of the *live
run's final data file* — i.e., the fully-advanced file reflecting all 300
operations' worth of incidental buffer-pool eviction. That combination
triggered a checksum-mismatch exception during recovery on an untouched
page. The bug wasn't in recovery — it was in the test. A real crash's
data file can only ever contain content that was flushed *after* being
logged (eviction only touches already-logged pages, by construction), so
a real crash's data file is always consistent with *its own* WAL's
durable prefix. Pairing an early, truncated WAL with a *late*, fully-run
data file manufactures a state that can never actually occur — it's
strictly "ahead of" what any real crash at that WAL offset could have
produced. The fix was to test against a **fresh, empty data file** for
every truncation point instead, which is both the physically realizable
scenario and the strictly stronger test: 100% of reconstruction has to
come from the WAL alone, nothing scavenged from data-file leftovers.
(This is also a direct demonstration of why `RunRecovery` doesn't need
the data file to be empty going in — `AllocatePage` and `NewPage` never
touch the data file directly, only `WritePage` does, so recovery is
fully self-sufficient from the WAL regardless of what, if anything, was
already on disk.)

### Real process kill (`crash_worker` + `test_crash_kill.cpp`)

The truncation tests above prove the recovery *logic* is correct against
any possible WAL prefix, but they never leave a single, cooperative,
in-process simulation — they can't rule out a bug where `fsync()`
doesn't actually mean what the code assumes it means, or where some
other part of the process's own cleanup was quietly required for
correctness.

`crash_worker` is a separate, minimal executable: open a `Database`,
perform a given number of complete `Insert()` calls, then call
`raise(SIGKILL)` on itself — no destructors, no explicit fd close, no
cooperative shutdown of any kind. `test_crash_kill.cpp` forks, execs the
worker, and — critically — asserts via `WIFSIGNALED`/`WTERMSIG` that the
child actually died **by SIGKILL**, not that it merely exited; a test
that skipped this check could pass for the wrong reason (a bug that
makes the worker exit early, rather than proof that data written before
a real, abrupt kill survives it). A genuinely separate process then opens
the same files and verifies full recovery, for 0, 1, 3, and 500 prior
inserts (the last forcing the same multi-level split structure the
truncation tests exercise).

## Known limitations, stated rather than hidden

- **No checkpointing or log truncation.** The WAL grows forever; nothing
  reclaims old segments once their pages are known-durable in the data
  file. A real system periodically checkpoints and discards.
- **Orphaned pages are never reclaimed.** A page created but never
  referenced by anything that survives a crash (or a non-rebalancing
  delete, per Milestone 1) is permanently wasted space — there's no
  free-list or garbage collection.
- **No UNDO.** As noted above, this WAL format is sufficient for
  REDO-only recovery but not for transactional rollback. Adding
  transactions later — explicitly a stretch goal, not core scope — would
  need a different record format.
- **No group commit / no concurrency.** Every mutation fsyncs alone. Fine
  at this project's scale; would be a real throughput problem otherwise.