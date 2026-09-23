#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "buffer_pool.h"

namespace reldb {

// A B+tree: all key/value data lives in leaves, internal nodes hold only
// routing separator keys, and leaves are chained via NodeExtra::next_leaf
// so range scans don't need to re-descend from the root for every key.
//
// Not thread-safe beyond what BufferPool itself guarantees (a single
// latch around frame bookkeeping) — concurrent tree structure
// modification needs real latch coupling, explicitly out of scope per the
// project's milestones.
class BTree {
 public:
  // On a fresh (zero-page) database, creates the metadata page and an
  // empty root leaf. On an existing one, reads root_page_id from page 0.
  explicit BTree(BufferPool* buffer_pool);

  // Inserts, or overwrites the value if the key already exists.
  // Throws std::invalid_argument if value.size() > VALUE_SIZE.
  void Insert(int64_t key, const std::string& value);

  // Returns true if the key existed and was removed. Does NOT rebalance
  // (merge/borrow) underfull nodes — a documented simplification; nodes
  // can end up sparser than a production B-tree would allow, but the
  // structure remains correct and searchable.
  bool Delete(int64_t key);

  std::optional<std::string> Get(int64_t key) const;

  // Inclusive on both ends.
  std::vector<std::pair<int64_t, std::string>> RangeScan(int64_t low,
                                                           int64_t high) const;

  page_id_t root_page_id() const { return root_page_id_; }

 private:
  struct SplitResult {
    int64_t promoted_key;
    page_id_t new_right_page_id;
  };

  std::optional<SplitResult> InsertRecursive(page_id_t node_id, int64_t key,
                                              const std::string& value);
  std::optional<SplitResult> SplitLeafIfNeeded(PageGuard& guard);
  std::optional<SplitResult> SplitInternalIfNeeded(PageGuard& guard);

  page_id_t FindLeafPageId(int64_t key) const;

  BufferPool* buffer_pool_;
  page_id_t metadata_page_id_ = 0;
  page_id_t root_page_id_;
};

}  // namespace reldb