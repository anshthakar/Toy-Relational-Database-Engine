#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace reldb {

using page_id_t = uint32_t;
constexpr page_id_t INVALID_PAGE_ID = static_cast<page_id_t>(-1);

constexpr size_t PAGE_SIZE = 4096;

// Page header layout, lives at the start of every page's byte buffer.
// Deliberately a plain struct with fixed-width fields (no pointers, no
// virtuals) so it can be memcpy'd to/from disk byte-for-byte.
enum class PageType : uint8_t {
  INVALID = 0,
  LEAF = 1,
  INTERNAL = 2,
  METADATA = 3,
};

#pragma pack(push, 1)
struct PageHeader {
  page_id_t page_id;
  PageType page_type;
  uint16_t num_slots;
  uint16_t free_space_ptr;  // offset (from page start) where free space begins
  uint32_t lsn;             // LSN of the last WAL record applied to this page
  uint32_t checksum;        // computed over the rest of the page on flush
  uint8_t reserved[7];      // pads header to 24 bytes (multiple of 8) so
                            // everything after it — B-tree cell arrays in
                            // particular — starts at an 8-byte-aligned
                            // offset. Without this, int64_t keys inside
                            // packed cell structs get read at misaligned
                            // addresses, undefined behavior that UBSan
                            // correctly flags even though x86 tolerates it.
};
#pragma pack(pop)

static_assert(sizeof(PageHeader) == 24,
              "header size must stay a multiple of 8 for downstream "
              "alignment — update NODE_EXTRA_OFFSET etc. if this changes");

// One in-memory page: a raw fixed-size byte buffer. Everything else
// (B-tree node interpretation, slot array, cells) is a view over these
// bytes, not a separate representation, so what you read from disk is
// exactly what you reason about in memory.
//
// alignas(8): guarantees every Page object starts on an 8-byte boundary
// regardless of where it's allocated (stack, heap, member of another
// struct). Combined with PageHeader being padded to a multiple of 8, this
// is what makes it well-defined to reinterpret_cast into the page and read
// 8-byte fields (B-tree keys) — without it, alignment would depend on
// allocator luck.
class alignas(8) Page {
 public:
  Page() { data_.fill(std::byte{0}); }

  std::byte* data() { return data_.data(); }
  const std::byte* data() const { return data_.data(); }
  static constexpr size_t size() { return PAGE_SIZE; }

  PageHeader* header() {
    return reinterpret_cast<PageHeader*>(data_.data());
  }
  const PageHeader* header() const {
    return reinterpret_cast<const PageHeader*>(data_.data());
  }

 private:
  std::array<std::byte, PAGE_SIZE> data_;
};

}  // namespace reldb