#pragma once

#include <cstdint>
#include <cstring>

#include "page.h"

namespace reldb {

// --- Documented simplifications (see the milestone 1 design doc) ---
// 1. Fixed-size values (VALUE_SIZE bytes), not the variable-length slotted
//    layout used for generic pages. This trades value-size flexibility for
//    a much simpler node format: cells are a plain sorted fixed-stride
//    array, no free-space pointer, no slot indirection.
// 2. Fanout is capped far below what a 4096-byte page could physically
//    hold (LEAF/INTERNAL_MAX_KEYS = 4), specifically so that a modest
//    number of test insertions triggers leaf splits, internal splits, and
//    multiple tree levels. A real engine would size this from the page,
//    not hardcode it.

constexpr size_t VALUE_SIZE = 32;
constexpr size_t LEAF_MAX_KEYS = 4;
constexpr size_t INTERNAL_MAX_KEYS = 4;

// Sits immediately after PageHeader on every leaf/internal page. Only one
// of the two fields is meaningful depending on page_type; the other is
// unused. Wasting 4 bytes to avoid a second page layout is a fine trade
// for a toy project.
#pragma pack(push, 1)
struct NodeExtra {
  page_id_t next_leaf;       // LEAF pages: sibling chain for range scans.
  page_id_t leftmost_child;  // INTERNAL pages: child before the first key.
};

struct LeafCell {
  int64_t key;
  uint16_t value_len;
  char value[VALUE_SIZE];
  uint8_t reserved[6];  // pads 8+2+32=42 up to 48 (multiple of 8) so every
                         // element of a LeafCell array keeps `key` aligned,
                         // not just the first one.
};

// child = page id of the subtree holding all keys > this cell's key (and,
// if there's a following cell, <= that cell's key). The child for keys
// less than every separator lives in NodeExtra::leftmost_child instead.
struct InternalCell {
  int64_t key;
  page_id_t child;
  uint8_t reserved[4];  // pads 8+4=12 up to 16 for the same reason.
};

// The one page (always page_id 0) that anchors the tree: everything else
// is reachable by walking from root_page_id.
struct MetadataBody {
  page_id_t root_page_id;
};
#pragma pack(pop)

constexpr size_t NODE_EXTRA_OFFSET = sizeof(PageHeader);
constexpr size_t NODE_DATA_OFFSET = NODE_EXTRA_OFFSET + sizeof(NodeExtra);
constexpr size_t METADATA_BODY_OFFSET = sizeof(PageHeader);

static_assert(NODE_DATA_OFFSET % 8 == 0,
              "cell array must start 8-byte aligned, or int64_t keys in "
              "LeafCell/InternalCell get read at misaligned addresses");
static_assert(sizeof(LeafCell) % 8 == 0,
              "cell stride must be a multiple of 8 to keep every array "
              "element's key field aligned, not just the first");
static_assert(sizeof(InternalCell) % 8 == 0, "same reasoning as LeafCell");

inline NodeExtra* GetNodeExtra(Page* page) {
  return reinterpret_cast<NodeExtra*>(page->data() + NODE_EXTRA_OFFSET);
}
inline LeafCell* LeafCells(Page* page) {
  return reinterpret_cast<LeafCell*>(page->data() + NODE_DATA_OFFSET);
}
inline InternalCell* InternalCells(Page* page) {
  return reinterpret_cast<InternalCell*>(page->data() + NODE_DATA_OFFSET);
}
inline MetadataBody* GetMetadata(Page* page) {
  return reinterpret_cast<MetadataBody*>(page->data() + METADATA_BODY_OFFSET);
}

}  // namespace reldb