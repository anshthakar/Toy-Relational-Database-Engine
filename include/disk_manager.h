#pragma once

#include <atomic>
#include <string>

#include "page.h"

namespace reldb {

// Owns the single on-disk file backing the database. Every method operates
// in units of whole pages at fixed offsets (page_id * PAGE_SIZE) — this is
// the ONLY class that knows about file descriptors and byte offsets. The
// buffer pool and B-tree never touch the file directly; they go through
// this so that when the WAL (milestone 2) needs raw durable writes too, the
// low-level I/O logic isn't duplicated or, worse, diverges.
class DiskManager {
 public:
  explicit DiskManager(const std::string& db_file);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads exactly one page's worth of bytes into `page`, verifying its
  // stored checksum. Throws std::runtime_error on a short read or checksum
  // mismatch — both indicate a torn write or corruption, and callers must
  // not silently treat that as valid data.
  void ReadPage(page_id_t page_id, Page* page);

  // Computes and stores the checksum, then writes the page at its offset.
  // Does not fsync — call Flush() for a durability barrier.
  void WritePage(page_id_t page_id, Page* page);

  // Extends the file by one page and returns its new page_id. The page's
  // on-disk bytes are whatever the OS gives a hole-extended file (typically
  // zero), the caller must initialize it via WritePage before reading it
  // back.
  page_id_t AllocatePage();

  // fsync — call this at points where you need a durability guarantee
  // (this becomes critical in milestone 2, where the WAL must be fsynced
  // before the mutation it describes is considered committed).
  void Flush();

  size_t NumPages() const;

  // Re-derives next_page_id_ from the current file size. Recovery applies
  // pages directly via WritePage() at their original page_id, which can
  // extend the file past whatever next_page_id_ was computed at
  // construction time (that snapshot predates recovery). Call this once,
  // immediately after recovery finishes replaying WAL records and before
  // any AllocatePage() call, or new pages could collide with page_ids
  // recovery just wrote.
  void RefreshNextPageIdAfterRecovery();

 private:
  int fd_;
  std::atomic<page_id_t> next_page_id_;
};

}  // namespace reldb