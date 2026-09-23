#include "wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace reldb {

namespace {

// Same FNV-1a approach as checksum.h, but over an arbitrary byte range
// rather than a whole Page — the WAL record header + payload aren't a
// Page-shaped object, so checksum.h's ComputeChecksum() doesn't apply
// directly.
uint32_t Fnv1a(const void* data, size_t len) {
  constexpr uint32_t FNV_OFFSET = 2166136261u;
  constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = FNV_OFFSET;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

uint32_t ComputeRecordChecksum(const WALRecordHeader& header,
                                const Page& page) {
  // Hash the header fields that precede `checksum` in the struct, then
  // the payload — deliberately NOT hashing the checksum field itself,
  // same reasoning as the page checksum: you can't checksum a field that
  // stores the checksum.
  uint32_t hash = Fnv1a(&header.magic, sizeof(header.magic));
  // Combine sequentially rather than hashing a padded/packed struct in
  // one call, so the result doesn't depend on struct layout details.
  uint8_t buf[sizeof(header.lsn) + sizeof(header.page_id)];
  std::memcpy(buf, &header.lsn, sizeof(header.lsn));
  std::memcpy(buf + sizeof(header.lsn), &header.page_id,
              sizeof(header.page_id));
  uint32_t h2 = Fnv1a(buf, sizeof(buf));
  hash = hash * 16777619u ^ h2;
  uint32_t h3 = Fnv1a(page.data(), Page::size());
  hash = hash * 16777619u ^ h3;
  return hash;
}

}  // namespace

WALManager::WALManager(const std::string& wal_path) {
  fd_ = open(wal_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("WALManager: failed to open " + wal_path + ": " +
                              std::strerror(errno));
  }

  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    throw std::runtime_error("WALManager: fstat failed: " +
                              std::string(std::strerror(errno)));
  }

  // Scan every complete WAL_RECORD_SIZE-sized slot from the start,
  // stopping at the first one that fails its checksum (or doesn't exist
  // — a short/partial trailing record). That first failure point is
  // exactly where a prior crash's torn write begins, so it's also where
  // this file gets truncated back to, below.
  uint64_t offset = 0;
  std::vector<char> raw(WAL_RECORD_SIZE);

  while (offset + WAL_RECORD_SIZE <= static_cast<uint64_t>(st.st_size)) {
    ssize_t n = pread(fd_, raw.data(), WAL_RECORD_SIZE,
                       static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(WAL_RECORD_SIZE)) {
      break;  // short read: treat as a torn tail, stop here
    }

    WALRecordHeader header;
    std::memcpy(&header, raw.data(), sizeof(WALRecordHeader));

    if (header.magic != WAL_MAGIC) {
      break;  // not a valid record start — garbage or torn write
    }

    RecoveredPage rp;
    rp.page_id = header.page_id;
    rp.lsn = header.lsn;
    std::memcpy(rp.page.data(), raw.data() + sizeof(WALRecordHeader),
                PAGE_SIZE);

    uint32_t computed = ComputeRecordChecksum(header, rp.page);
    if (computed != header.checksum) {
      break;  // checksum mismatch — torn write, stop here
    }

    valid_records_.push_back(std::move(rp));
    next_lsn_ = std::max(next_lsn_, header.lsn + 1);
    offset += WAL_RECORD_SIZE;
  }

  // Truncate away anything at or past `offset` — a torn/garbage tail (or
  // a short trailing chunk smaller than one record) from a prior crash.
  // Without this, future appends would land after that torn data, and
  // since a future recovery scan always stops at the FIRST invalid
  // record, everything we're about to append would become permanently
  // unreachable.
  if (static_cast<uint64_t>(st.st_size) != offset) {
    if (ftruncate(fd_, static_cast<off_t>(offset)) != 0) {
      throw std::runtime_error("WALManager: ftruncate failed: " +
                                std::string(std::strerror(errno)));
    }
  }

  next_offset_ = offset;
}

WALManager::~WALManager() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

uint64_t WALManager::AppendRecord(page_id_t page_id, Page* page) {
  uint64_t lsn = next_lsn_++;
  page->header()->lsn = static_cast<uint32_t>(lsn);

  WALRecordHeader header;
  header.magic = WAL_MAGIC;
  header.lsn = lsn;
  header.page_id = page_id;
  header.checksum = ComputeRecordChecksum(header, *page);

  std::vector<char> raw(WAL_RECORD_SIZE);
  std::memcpy(raw.data(), &header, sizeof(WALRecordHeader));
  std::memcpy(raw.data() + sizeof(WALRecordHeader), page->data(), PAGE_SIZE);

  ssize_t written = pwrite(fd_, raw.data(), WAL_RECORD_SIZE,
                            static_cast<off_t>(next_offset_));
  if (written != static_cast<ssize_t>(WAL_RECORD_SIZE)) {
    throw std::runtime_error("WALManager: short write appending WAL record");
  }

  // The durability barrier: the caller (BufferPool, on unpin of a dirty
  // page) does not get control back until this returns. This is what
  // makes "logged" mean "actually on disk," not just "handed to the OS
  // page cache."
  if (fsync(fd_) != 0) {
    throw std::runtime_error("WALManager: fsync failed: " +
                              std::string(std::strerror(errno)));
  }

  next_offset_ += WAL_RECORD_SIZE;

  // Keep valid_records_ current, not just "as of the last open" — the
  // fsync above just confirmed this record itself is durable, so there
  // is no reason ValidRecords() should lag behind what AppendRecord has
  // already returned from. This also means a page logged more than once
  // in one process's lifetime appears more than once here, same as it
  // would after a fresh open-and-rescan; callers (recovery) already
  // reduce to "latest LSN per page_id", so the duplicate is harmless.
  RecoveredPage rp;
  rp.page_id = page_id;
  rp.lsn = lsn;
  rp.page = *page;
  valid_records_.push_back(std::move(rp));

  return lsn;
}

}  // namespace reldb