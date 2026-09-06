#include "disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

#include "checksum.h"

namespace reldb {

DiskManager::DiskManager(const std::string& db_file) {
  fd_ = open(db_file.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    throw std::runtime_error("DiskManager: failed to open " + db_file + ": " +
                              std::strerror(errno));
  }

  struct stat st{};
  if (fstat(fd_, &st) != 0) {
    throw std::runtime_error("DiskManager: fstat failed: " +
                              std::string(std::strerror(errno)));
  }
  next_page_id_ = static_cast<page_id_t>(st.st_size / PAGE_SIZE);
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

void DiskManager::ReadPage(page_id_t page_id, Page* page) {
  const off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t bytes_read = pread(fd_, page->data(), PAGE_SIZE, offset);
  if (bytes_read != static_cast<ssize_t>(PAGE_SIZE)) {
    throw std::runtime_error(
        "DiskManager: short read on page " + std::to_string(page_id) +
        " (got " + std::to_string(bytes_read) + " bytes) — file truncated?");
  }

  uint32_t stored_checksum = page->header()->checksum;
  uint32_t computed = ComputeChecksum(*page);
  if (stored_checksum != computed) {
    throw std::runtime_error(
        "DiskManager: checksum mismatch on page " + std::to_string(page_id) +
        " — page is corrupted or was torn during a write");
  }
}

void DiskManager::WritePage(page_id_t page_id, Page* page) {
  page->header()->checksum = ComputeChecksum(*page);

  const off_t offset = static_cast<off_t>(page_id) * PAGE_SIZE;
  ssize_t bytes_written = pwrite(fd_, page->data(), PAGE_SIZE, offset);
  if (bytes_written != static_cast<ssize_t>(PAGE_SIZE)) {
    throw std::runtime_error(
        "DiskManager: short write on page " + std::to_string(page_id) +
        " (wrote " + std::to_string(bytes_written) + " bytes)");
  }
}

page_id_t DiskManager::AllocatePage() {
  return next_page_id_.fetch_add(1);
}

void DiskManager::Flush() {
  if (fsync(fd_) != 0) {
    throw std::runtime_error("DiskManager: fsync failed: " +
                              std::string(std::strerror(errno)));
  }
}

size_t DiskManager::NumPages() const {
  return next_page_id_.load();
}

}  // namespace reldb