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
    };
#pragma pack(pop)

    static_assert(sizeof(PageHeader) <= 32,
                  "keep the header small; it eats into usable page space");

    // One in-memory page: a raw fixed-size byte buffer. Everything else
    // (B-tree node interpretation, slot array, cells) is a view over these
    // bytes, not a separate representation, so what you read from disk is
    // exactly what you reason about in memory.
    class Page {
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