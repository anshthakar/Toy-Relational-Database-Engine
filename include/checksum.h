#pragma once

#include <cstddef>
#include <cstdint>

#include "page.h"

namespace reldb {

    inline uint32_t ComputeChecksum(const Page& page) {
        constexpr uint32_t FNV_OFFSET = 2166136261u;
        constexpr uint32_t FNV_PRIME = 16777619u;

        uint32_t hash = FNV_OFFSET;
        const std::byte* bytes = page.data();
        const size_t checksum_offset = offsetof(PageHeader, checksum);

        for (size_t i = 0; i < Page::size(); ++i) {
            if (i >= checksum_offset && i < checksum_offset + sizeof(uint32_t)) {
                continue;
            }
            hash ^= static_cast<uint8_t>(bytes[i]);
            hash *= FNV_PRIME;
        }
        return hash;
    }

}  // namespace reldbwsl --install