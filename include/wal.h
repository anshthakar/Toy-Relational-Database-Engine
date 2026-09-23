#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "page.h"

namespace reldb {

// --- Documented simplifications (see docs/milestone-2.md) ---
// 1. Each record logs the FULL after-image of one page (~4KB), not a
//    byte-level diff. Real WALs log small deltas to stay compact; this
//    makes recovery close to trivial (just re-write the bytes, no replay
//    ordering to reconstruct) at the cost of a much larger log. Right
//    tradeoff for understanding *why* WAL works, wrong one for a real
//    engine's write throughput.
// 2. Every append fsyncs immediately — no group commit / batching across
//    concurrent operations. There's no concurrency yet, so batching would
//    add real complexity for no measurable benefit at this stage.
// 3. REDO-only. There's no transaction/rollback concept yet, so nothing
//    logged is ever "uncommitted" — every record represents a mutation
//    that should exist. If transactions are added later (explicit stretch
//    goal), this format alone is not sufficient; UNDO records would be
//    needed too.

constexpr uint32_t WAL_MAGIC = 0x57414C31;  // "WAL1"

#pragma pack(push, 1)
struct WALRecordHeader {
  uint32_t magic;
  uint64_t lsn;
  page_id_t page_id;
  uint32_t checksum;  // over {magic, lsn, page_id, payload} — everything
                       // in the record except this field itself.
};
#pragma pack(pop)

constexpr size_t WAL_RECORD_SIZE = sizeof(WALRecordHeader) + PAGE_SIZE;

class WALManager {
 public:
  // Opens (or creates) the WAL file. If the file's tail contains a
  // record that fails its checksum — a torn write from a prior crash —
  // the file is truncated back to the last fully valid record right
  // here, in the constructor. This matters beyond just "clean state":
  // if we didn't do this, future appends would land AFTER the torn
  // record, and since recovery always stops scanning at the first
  // invalid record it finds, everything appended after a torn tail
  // would be permanently unreachable to any future recovery scan, even
  // though it was written correctly. Truncating first is what makes the
  // file safe to keep appending to.
  explicit WALManager(const std::string& wal_path);
  ~WALManager();

  WALManager(const WALManager&) = delete;
  WALManager& operator=(const WALManager&) = delete;

  // Stamps page.header()->lsn with a freshly assigned LSN, writes a
  // record containing that page's current bytes, and fsyncs before
  // returning. The caller only gets control back once this mutation is
  // durable. Returns the assigned LSN.
  uint64_t AppendRecord(page_id_t page_id, Page* page);

  struct RecoveredPage {
    page_id_t page_id;
    uint64_t lsn;
    Page page;
  };

  // Every valid record known to this WALManager so far, in file order:
  // whatever was found valid at open time (already truncated clean of
  // any torn tail), plus anything appended since via AppendRecord() in
  // this same process. This is the input to recovery: reduce to "latest
  // record per page_id" and replay those pages onto the data file — and
  // since recovery always constructs a fresh WALManager immediately
  // before any appends happen, it only ever sees the open-time portion
  // in practice, but the live view is kept accurate regardless.
  const std::vector<RecoveredPage>& ValidRecords() const {
    return valid_records_;
  }

  uint64_t NextLsn() const { return next_lsn_; }

 private:
  int fd_;
  uint64_t next_offset_ = 0;
  uint64_t next_lsn_ = 0;
  std::vector<RecoveredPage> valid_records_;
};

}  // namespace reldb