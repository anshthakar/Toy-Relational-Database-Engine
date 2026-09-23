#pragma once

#include <cstddef>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "disk_manager.h"
#include "page.h"
#include "wal.h"

namespace reldb {

using frame_id_t = int32_t;
constexpr frame_id_t INVALID_FRAME_ID = -1;

// One in-memory slot. Owns the actual Page bytes; page_id/dirty/pin_count
// describe what's currently loaded into it. A frame with page_id ==
// INVALID_PAGE_ID is empty (never used or its previous contents were
// evicted and flushed).
struct Frame {
  Page page;
  page_id_t page_id = INVALID_PAGE_ID;
  int pin_count = 0;
  bool dirty = false;
};

class BufferPool;

// RAII handle to a pinned page. As long as this object is alive, the
// underlying frame cannot be evicted. Destruction unpins automatically —
// this is the mechanism, not a convention, that prevents "forgot to unpin"
// bugs: there is no code path that holds a pin without also holding a
// PageGuard whose destructor will release it.
//
// Move-only: copying would let two guards both believe they own the same
// pin, and then both destructors would unpin, double-releasing it.
class PageGuard {
 public:
  PageGuard() = default;
  PageGuard(BufferPool* pool, page_id_t page_id, Frame* frame);
  ~PageGuard();

  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  PageGuard(PageGuard&& other) noexcept;
  PageGuard& operator=(PageGuard&& other) noexcept;

  Page* page() { return frame_ ? &frame_->page : nullptr; }
  const Page* page() const { return frame_ ? &frame_->page : nullptr; }
  page_id_t page_id() const { return page_id_; }
  bool valid() const { return frame_ != nullptr; }

  // Call this whenever you mutate the page's bytes through page(). Without
  // it, the buffer pool won't know to flush this frame's changes back to
  // disk before evicting or reusing it, and mutations would be silently
  // lost. It is on the caller to call this, the same way it's on the
  // caller to actually write through page() — there's no way to
  // automatically detect a byte-level mutation.
  void MarkDirty();

 private:
  void Release();

  BufferPool* pool_ = nullptr;
  page_id_t page_id_ = INVALID_PAGE_ID;
  Frame* frame_ = nullptr;
};

// Fixed-capacity in-memory cache of pages backed by a DiskManager. Never
// touches the file directly — all disk I/O goes through DiskManager, kept
// as a strict layering: BufferPool doesn't know about byte offsets or
// checksums, DiskManager doesn't know about pinning or eviction.
class BufferPool {
 public:
  // wal_manager is optional (nullptr = no WAL logging — used by
  // milestone 1's tests, which predate the WAL and don't need crash
  // durability). When non-null, every page that gets unpinned dirty is
  // logged — full after-image, fsynced — BEFORE this call returns. That
  // is the actual durability guarantee: a mutation is never considered
  // "done" from the caller's point of view until it is safely logged,
  // not merely applied to the in-memory frame.
  BufferPool(size_t pool_size, DiskManager* disk_manager,
             WALManager* wal_manager = nullptr);

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Loads an existing page into memory (or returns it if already cached),
  // pins it, and returns a guard. Returns an invalid guard (valid() ==
  // false) if the pool is full and every frame is currently pinned — there
  // is nothing to evict.
  PageGuard FetchPage(page_id_t page_id);

  // Allocates a brand new page via the DiskManager, pins it, and returns
  // a guard over a zero-initialized in-memory page. The caller is
  // responsible for writing real content and calling MarkDirty() so it
  // gets flushed.
  PageGuard NewPage();

  // Writes a specific page back to disk immediately, regardless of dirty
  // state. Mostly useful for tests and explicit checkpoints; ordinary
  // eviction handles this automatically for dirty frames.
  bool FlushPage(page_id_t page_id);

  void FlushAll();

  size_t PoolSize() const { return frames_.size(); }
  size_t FreeFrameCount() const { return free_list_.size(); }

  // Passthrough so higher layers (BTree) never need to hold a separate
  // DiskManager reference just to ask "does this file have any pages
  // yet" during initialization — keeps the layering strict (BTree only
  // ever talks to BufferPool).
  size_t DiskPageCount() const { return disk_manager_->NumPages(); }

 private:
  friend class PageGuard;

  // Called by PageGuard's destructor. Not part of the public API — pins
  // and unpins must always be paired through a guard's lifetime, never
  // called directly.
  void UnpinPage(page_id_t page_id, bool is_dirty);

  // Finds a frame to use for a new page: prefers the free list, falls back
  // to evicting the LRU-tail frame with pin_count == 0. Returns
  // INVALID_FRAME_ID if nothing is evictable.
  frame_id_t FindVictimFrame();

  void TouchLRU(frame_id_t frame_id);   // move to front (most recent)
  void RemoveFromLRU(frame_id_t frame_id);

  std::vector<Frame> frames_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;

  std::list<frame_id_t> lru_list_;  // front = most recently used
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> lru_iters_;

  DiskManager* disk_manager_;
  WALManager* wal_manager_;  // nullptr if this pool logs nothing
  std::mutex latch_;
};

}  // namespace reldb