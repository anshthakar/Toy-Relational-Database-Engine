#include "buffer_pool.h"

#include <stdexcept>

namespace reldb {

// ---------- PageGuard ----------

PageGuard::PageGuard(BufferPool* pool, page_id_t page_id, Frame* frame)
    : pool_(pool), page_id_(page_id), frame_(frame) {}

PageGuard::~PageGuard() { Release(); }

PageGuard::PageGuard(PageGuard&& other) noexcept
    : pool_(other.pool_), page_id_(other.page_id_), frame_(other.frame_) {
  other.pool_ = nullptr;
  other.frame_ = nullptr;
  other.page_id_ = INVALID_PAGE_ID;
}

PageGuard& PageGuard::operator=(PageGuard&& other) noexcept {
  if (this != &other) {
    Release();  // release whatever this guard currently holds, if anything
    pool_ = other.pool_;
    page_id_ = other.page_id_;
    frame_ = other.frame_;
    other.pool_ = nullptr;
    other.frame_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
  }
  return *this;
}

void PageGuard::MarkDirty() {
  if (frame_) {
    frame_->dirty = true;
  }
}

void PageGuard::Release() {
  if (pool_ && frame_) {
    pool_->UnpinPage(page_id_, frame_->dirty);
  }
  pool_ = nullptr;
  frame_ = nullptr;
  page_id_ = INVALID_PAGE_ID;
}

// ---------- BufferPool ----------

BufferPool::BufferPool(size_t pool_size, DiskManager* disk_manager)
    : frames_(pool_size), disk_manager_(disk_manager) {
  for (size_t i = 0; i < pool_size; ++i) {
    free_list_.push_back(static_cast<frame_id_t>(i));
  }
}

void BufferPool::TouchLRU(frame_id_t frame_id) {
  auto it = lru_iters_.find(frame_id);
  if (it != lru_iters_.end()) {
    lru_list_.erase(it->second);
  }
  lru_list_.push_front(frame_id);
  lru_iters_[frame_id] = lru_list_.begin();
}

void BufferPool::RemoveFromLRU(frame_id_t frame_id) {
  auto it = lru_iters_.find(frame_id);
  if (it != lru_iters_.end()) {
    lru_list_.erase(it->second);
    lru_iters_.erase(it);
  }
}

frame_id_t BufferPool::FindVictimFrame() {
  if (!free_list_.empty()) {
    frame_id_t id = free_list_.back();
    free_list_.pop_back();
    return id;
  }

  // Scan from the back (least recently used end) for the first frame with
  // no active pins. A toy-project simplification: this is O(n) in the
  // worst case rather than tracking an "unpinned tail" separately, which
  // is fine at the frame counts this project runs at.
  for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
    frame_id_t candidate = *it;
    Frame& frame = frames_[candidate];
    if (frame.pin_count == 0) {
      RemoveFromLRU(candidate);

      if (frame.dirty) {
        disk_manager_->WritePage(frame.page_id, &frame.page);
      }
      page_table_.erase(frame.page_id);
      return candidate;
    }
  }

  return INVALID_FRAME_ID;  // every frame is pinned; nothing evictable
}

PageGuard BufferPool::FetchPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second;
    Frame& frame = frames_[frame_id];
    frame.pin_count++;
    TouchLRU(frame_id);
    return PageGuard(this, page_id, &frame);
  }

  frame_id_t frame_id = FindVictimFrame();
  if (frame_id == INVALID_FRAME_ID) {
    return PageGuard();  // pool exhausted, nothing pinnable
  }

  Frame& frame = frames_[frame_id];
  disk_manager_->ReadPage(page_id, &frame.page);  // propagates on corruption
  frame.page_id = page_id;
  frame.pin_count = 1;
  frame.dirty = false;

  page_table_[page_id] = frame_id;
  TouchLRU(frame_id);

  return PageGuard(this, page_id, &frame);
}

PageGuard BufferPool::NewPage() {
  std::lock_guard<std::mutex> lock(latch_);

  frame_id_t frame_id = FindVictimFrame();
  if (frame_id == INVALID_FRAME_ID) {
    return PageGuard();
  }

  page_id_t new_id = disk_manager_->AllocatePage();

  Frame& frame = frames_[frame_id];
  frame.page = Page();  // reset to a zeroed page
  frame.page.header()->page_id = new_id;
  frame.page_id = new_id;
  frame.pin_count = 1;
  // Dirty from the moment it exists: its content only lives in memory so
  // far, disk has never been told about this page id's contents. If it
  // gets evicted before an explicit flush, eviction must write it out.
  frame.dirty = true;

  page_table_[new_id] = frame_id;
  TouchLRU(frame_id);

  return PageGuard(this, new_id, &frame);
}

void BufferPool::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return;  // already evicted somehow; nothing to unpin
  }

  Frame& frame = frames_[it->second];
  if (is_dirty) {
    frame.dirty = true;
  }
  if (frame.pin_count > 0) {
    frame.pin_count--;
  }
}

bool BufferPool::FlushPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;
  }

  Frame& frame = frames_[it->second];
  disk_manager_->WritePage(page_id, &frame.page);
  frame.dirty = false;
  return true;
}

void BufferPool::FlushAll() {
  std::lock_guard<std::mutex> lock(latch_);

  for (auto& [page_id, frame_id] : page_table_) {
    Frame& frame = frames_[frame_id];
    if (frame.dirty) {
      disk_manager_->WritePage(page_id, &frame.page);
      frame.dirty = false;
    }
  }
}

}  // namespace reldb