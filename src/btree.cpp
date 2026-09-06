#include "btree.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "btree_node.h"

namespace reldb {

BTree::BTree(BufferPool* buffer_pool) : buffer_pool_(buffer_pool) {
  if (buffer_pool_->DiskPageCount() == 0) {
    // Fresh database: page 0 is always metadata, page 1 is the initial
    // empty root leaf. Order matters — metadata must be allocated first
    // so it lands on page 0.
    PageGuard meta_guard = buffer_pool_->NewPage();
    meta_guard.page()->header()->page_type = PageType::METADATA;

    PageGuard root_guard = buffer_pool_->NewPage();
    root_guard.page()->header()->page_type = PageType::LEAF;
    root_guard.page()->header()->num_slots = 0;
    GetNodeExtra(root_guard.page())->next_leaf = INVALID_PAGE_ID;
    root_guard.MarkDirty();

    GetMetadata(meta_guard.page())->root_page_id = root_guard.page_id();
    meta_guard.MarkDirty();

    metadata_page_id_ = meta_guard.page_id();
    root_page_id_ = root_guard.page_id();
  } else {
    metadata_page_id_ = 0;
    PageGuard meta_guard = buffer_pool_->FetchPage(metadata_page_id_);
    if (!meta_guard.valid()) {
      throw std::runtime_error("BTree: could not read metadata page");
    }
    root_page_id_ = GetMetadata(meta_guard.page())->root_page_id;
  }
}

// ---------- Insert ----------

void BTree::Insert(int64_t key, const std::string& value) {
  if (value.size() > VALUE_SIZE) {
    throw std::invalid_argument("BTree::Insert: value exceeds VALUE_SIZE");
  }

  auto split = InsertRecursive(root_page_id_, key, value);
  if (!split.has_value()) {
    return;
  }

  // The root itself split. Wrap it in a brand new root so the tree grows
  // upward rather than needing every existing page to move.
  PageGuard new_root_guard = buffer_pool_->NewPage();
  if (!new_root_guard.valid()) {
    throw std::runtime_error("BTree::Insert: buffer pool exhausted");
  }
  Page* rp = new_root_guard.page();
  rp->header()->page_type = PageType::INTERNAL;
  rp->header()->num_slots = 1;
  GetNodeExtra(rp)->leftmost_child = root_page_id_;
  InternalCells(rp)[0] = {split->promoted_key, split->new_right_page_id, {}};
  new_root_guard.MarkDirty();

  root_page_id_ = new_root_guard.page_id();

  PageGuard meta_guard = buffer_pool_->FetchPage(metadata_page_id_);
  GetMetadata(meta_guard.page())->root_page_id = root_page_id_;
  meta_guard.MarkDirty();
}

std::optional<BTree::SplitResult> BTree::InsertRecursive(
    page_id_t node_id, int64_t key, const std::string& value) {
  PageGuard guard = buffer_pool_->FetchPage(node_id);
  if (!guard.valid()) {
    throw std::runtime_error("BTree::Insert: buffer pool exhausted");
  }
  Page* page = guard.page();

  if (page->header()->page_type == PageType::LEAF) {
    LeafCell* cells = LeafCells(page);
    uint16_t num = page->header()->num_slots;

    // Find sorted insert position (or the existing cell, for an upsert).
    size_t idx = 0;
    while (idx < num && cells[idx].key < key) idx++;

    if (idx < num && cells[idx].key == key) {
      cells[idx].value_len = static_cast<uint16_t>(value.size());
      std::memcpy(cells[idx].value, value.data(), value.size());
      guard.MarkDirty();
      return std::nullopt;  // update never causes a split
    }

    // Shift everything from idx onward right by one cell, then insert.
    // Physical page capacity is far larger than LEAF_MAX_KEYS by design
    // (see btree_node.h), so this is always safe even when it pushes the
    // logical count one past the split threshold.
    std::memmove(&cells[idx + 1], &cells[idx],
                 (num - idx) * sizeof(LeafCell));
    cells[idx].key = key;
    cells[idx].value_len = static_cast<uint16_t>(value.size());
    std::memcpy(cells[idx].value, value.data(), value.size());
    page->header()->num_slots = num + 1;
    guard.MarkDirty();

    return SplitLeafIfNeeded(guard);
  }

  // Internal node: find which child to descend into.
  NodeExtra* extra = GetNodeExtra(page);
  InternalCell* cells = InternalCells(page);
  uint16_t num = page->header()->num_slots;

  size_t idx = 0;
  while (idx < num && key >= cells[idx].key) idx++;
  page_id_t child_id = (idx == 0) ? extra->leftmost_child : cells[idx - 1].child;

  // `guard` stays pinned (and alive) across this recursive call. That's
  // deliberate: it's the pin, not any explicit lock, that stops this
  // internal node's frame from being evicted out from under us while
  // we're mid-insert lower in the tree.
  auto split = InsertRecursive(child_id, key, value);
  if (!split.has_value()) {
    return std::nullopt;
  }

  // A child split; insert its promoted separator + new right sibling into
  // this node at the position we just descended through.
  // Re-derive pointers: `page` is still valid (still pinned via `guard`,
  // never evicted), but re-reading num_slots defends against relying on a
  // stale local copy after the recursive call touched this same page.
  num = page->header()->num_slots;
  cells = InternalCells(page);
  std::memmove(&cells[idx + 1], &cells[idx], (num - idx) * sizeof(InternalCell));
  cells[idx] = {split->promoted_key, split->new_right_page_id, {}};
  page->header()->num_slots = num + 1;
  guard.MarkDirty();

  return SplitInternalIfNeeded(guard);
}

std::optional<BTree::SplitResult> BTree::SplitLeafIfNeeded(PageGuard& guard) {
  Page* page = guard.page();
  uint16_t num = page->header()->num_slots;
  if (num <= LEAF_MAX_KEYS) {
    return std::nullopt;
  }

  PageGuard right_guard = buffer_pool_->NewPage();
  if (!right_guard.valid()) {
    throw std::runtime_error("BTree: buffer pool exhausted during split");
  }
  Page* right = right_guard.page();
  right->header()->page_type = PageType::LEAF;

  LeafCell* left_cells = LeafCells(page);
  LeafCell* right_cells = LeafCells(right);

  size_t split_point = num / 2;
  size_t right_count = num - split_point;
  std::memcpy(right_cells, &left_cells[split_point],
              right_count * sizeof(LeafCell));

  right->header()->num_slots = static_cast<uint16_t>(right_count);
  page->header()->num_slots = static_cast<uint16_t>(split_point);

  NodeExtra* left_extra = GetNodeExtra(page);
  NodeExtra* right_extra = GetNodeExtra(right);
  right_extra->next_leaf = left_extra->next_leaf;
  left_extra->next_leaf = right_guard.page_id();

  guard.MarkDirty();
  right_guard.MarkDirty();

  return SplitResult{right_cells[0].key, right_guard.page_id()};
}

std::optional<BTree::SplitResult> BTree::SplitInternalIfNeeded(
    PageGuard& guard) {
  Page* page = guard.page();
  uint16_t num = page->header()->num_slots;
  if (num <= INTERNAL_MAX_KEYS) {
    return std::nullopt;
  }

  PageGuard right_guard = buffer_pool_->NewPage();
  if (!right_guard.valid()) {
    throw std::runtime_error("BTree: buffer pool exhausted during split");
  }
  Page* right = right_guard.page();
  right->header()->page_type = PageType::INTERNAL;

  InternalCell* left_cells = InternalCells(page);
  InternalCell* right_cells = InternalCells(right);

  // The middle cell's key moves UP to the parent rather than staying in
  // either child — internal nodes don't duplicate keys the way leaves'
  // separators do.
  size_t split_point = num / 2;
  int64_t promoted_key = left_cells[split_point].key;

  GetNodeExtra(right)->leftmost_child = left_cells[split_point].child;
  size_t right_count = num - split_point - 1;
  std::memcpy(right_cells, &left_cells[split_point + 1],
              right_count * sizeof(InternalCell));

  right->header()->num_slots = static_cast<uint16_t>(right_count);
  page->header()->num_slots = static_cast<uint16_t>(split_point);

  guard.MarkDirty();
  right_guard.MarkDirty();

  return SplitResult{promoted_key, right_guard.page_id()};
}

// ---------- Lookup / descent ----------

page_id_t BTree::FindLeafPageId(int64_t key) const {
  page_id_t cur = root_page_id_;
  while (true) {
    PageGuard guard = buffer_pool_->FetchPage(cur);
    if (!guard.valid()) {
      throw std::runtime_error("BTree: buffer pool exhausted during lookup");
    }
    Page* page = guard.page();
    if (page->header()->page_type == PageType::LEAF) {
      return cur;
    }
    NodeExtra* extra = GetNodeExtra(page);
    InternalCell* cells = InternalCells(page);
    uint16_t num = page->header()->num_slots;

    size_t idx = 0;
    while (idx < num && key >= cells[idx].key) idx++;
    cur = (idx == 0) ? extra->leftmost_child : cells[idx - 1].child;
    // guard drops at the end of this iteration — safe for a read-only,
    // single-threaded descent; nothing else can mutate the tree between
    // here and the next fetch.
  }
}

std::optional<std::string> BTree::Get(int64_t key) const {
  page_id_t leaf_id = FindLeafPageId(key);
  PageGuard guard = buffer_pool_->FetchPage(leaf_id);
  Page* page = guard.page();
  LeafCell* cells = LeafCells(page);
  uint16_t num = page->header()->num_slots;

  for (uint16_t i = 0; i < num; ++i) {
    if (cells[i].key == key) {
      return std::string(cells[i].value, cells[i].value_len);
    }
  }
  return std::nullopt;
}

bool BTree::Delete(int64_t key) {
  page_id_t leaf_id = FindLeafPageId(key);
  PageGuard guard = buffer_pool_->FetchPage(leaf_id);
  Page* page = guard.page();
  LeafCell* cells = LeafCells(page);
  uint16_t num = page->header()->num_slots;

  for (uint16_t i = 0; i < num; ++i) {
    if (cells[i].key == key) {
      std::memmove(&cells[i], &cells[i + 1],
                   (num - i - 1) * sizeof(LeafCell));
      page->header()->num_slots = num - 1;
      guard.MarkDirty();
      return true;
    }
  }
  return false;
}

std::vector<std::pair<int64_t, std::string>> BTree::RangeScan(
    int64_t low, int64_t high) const {
  std::vector<std::pair<int64_t, std::string>> results;
  if (low > high) return results;

  page_id_t leaf_id = FindLeafPageId(low);

  while (leaf_id != INVALID_PAGE_ID) {
    PageGuard guard = buffer_pool_->FetchPage(leaf_id);
    Page* page = guard.page();
    LeafCell* cells = LeafCells(page);
    uint16_t num = page->header()->num_slots;

    bool past_high = false;
    for (uint16_t i = 0; i < num; ++i) {
      if (cells[i].key < low) continue;
      if (cells[i].key > high) {
        past_high = true;
        break;
      }
      results.emplace_back(cells[i].key,
                            std::string(cells[i].value, cells[i].value_len));
    }

    if (past_high) break;
    leaf_id = GetNodeExtra(page)->next_leaf;
  }

  return results;
}

}  // namespace reldb