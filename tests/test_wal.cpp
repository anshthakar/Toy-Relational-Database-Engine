#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "wal.h"

using namespace reldb;

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      throw std::runtime_error(std::string("CHECK failed: ") + #cond +   \
                                " at " + __FILE__ + ":" +                \
                                std::to_string(__LINE__));                \
    }                                                                    \
  } while (0)

namespace {

std::string TempPath(const std::string& name) {
  return "/tmp/reldb_wal_test_" + name + ".log";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

Page MakePage(page_id_t id, const char* marker) {
  Page p;
  p.header()->page_id = id;
  std::memcpy(p.data() + sizeof(PageHeader), marker, std::strlen(marker) + 1);
  return p;
}

bool MarkerMatches(const Page& p, const char* marker) {
  return std::strcmp(reinterpret_cast<const char*>(p.data() +
                                                     sizeof(PageHeader)),
                      marker) == 0;
}

void TestAppendAndReopen() {
  std::string path = TempPath("append");
  RemoveIfExists(path);

  {
    WALManager wal(path);
    Page p1 = MakePage(5, "first");
    Page p2 = MakePage(6, "second");
    uint64_t lsn1 = wal.AppendRecord(5, &p1);
    uint64_t lsn2 = wal.AppendRecord(6, &p2);
    CHECK(lsn2 == lsn1 + 1);
    CHECK(wal.ValidRecords().size() == 2);
  }  // WALManager destructs — simulates process exit; file already fsynced

  {
    WALManager wal(path);  // fresh instance, re-scans the file from disk
    CHECK(wal.ValidRecords().size() == 2);
    CHECK(wal.ValidRecords()[0].page_id == 5);
    CHECK(MarkerMatches(wal.ValidRecords()[0].page, "first"));
    CHECK(wal.ValidRecords()[1].page_id == 6);
    CHECK(MarkerMatches(wal.ValidRecords()[1].page, "second"));
    CHECK(wal.NextLsn() == 2);
  }

  RemoveIfExists(path);
}

void TestTornTailIsTruncatedOnOpen() {
  std::string path = TempPath("torn");
  RemoveIfExists(path);

  {
    WALManager wal(path);
    Page p1 = MakePage(1, "good record");
    wal.AppendRecord(1, &p1);
  }

  // Simulate a crash mid-append: append a second, INTACT-looking record,
  // then chop a few bytes off the end — a torn write, exactly what a
  // real crash mid-pwrite/mid-fsync could leave behind.
  {
    int fd = open(path.c_str(), O_RDWR);
    CHECK(fd >= 0);
    off_t good_size = lseek(fd, 0, SEEK_END);

    WALRecordHeader header{};
    header.magic = WAL_MAGIC;
    header.lsn = 99;
    header.page_id = 2;
    header.checksum = 0xDEADBEEF;  // bogus on purpose — irrelevant, since
                                    // we're about to truncate before this
                                    // record is even complete
    std::vector<char> raw(WAL_RECORD_SIZE, 0);
    std::memcpy(raw.data(), &header, sizeof(header));
    CHECK(pwrite(fd, raw.data(), raw.size(), good_size) ==
          static_cast<ssize_t>(raw.size()));

    // Now truncate away the last third of that second record, simulating
    // a write that was cut off partway through.
    off_t torn_size = good_size + static_cast<off_t>(WAL_RECORD_SIZE * 2 / 3);
    CHECK(ftruncate(fd, torn_size) == 0);
    close(fd);
  }

  {
    WALManager wal(path);
    // Only the first, fully-intact record should survive.
    CHECK(wal.ValidRecords().size() == 1);
    CHECK(wal.ValidRecords()[0].page_id == 1);
    CHECK(MarkerMatches(wal.ValidRecords()[0].page, "good record"));
  }

  // And the file on disk should have actually been truncated back —
  // otherwise a future append would land after the torn garbage, making
  // it permanently unreachable to any later recovery scan (see the
  // reasoning in wal.h).
  {
    int fd = open(path.c_str(), O_RDONLY);
    off_t size = lseek(fd, 0, SEEK_END);
    close(fd);
    CHECK(static_cast<size_t>(size) == WAL_RECORD_SIZE);
  }

  RemoveIfExists(path);
}

void TestAppendAfterTruncationIsReachable() {
  // Directly exercises the reasoning in the comment above: after a torn
  // tail is truncated away on open, a fresh append must land where the
  // torn data used to start, not after it — otherwise recovery would
  // permanently skip anything appended post-recovery.
  std::string path = TempPath("append_after_truncate");
  RemoveIfExists(path);

  {
    WALManager wal(path);
    Page p1 = MakePage(1, "before crash");
    wal.AppendRecord(1, &p1);
  }

  {
    int fd = open(path.c_str(), O_RDWR);
    off_t good_size = lseek(fd, 0, SEEK_END);
    std::vector<char> garbage(WAL_RECORD_SIZE / 2, static_cast<char>(0xFF));
    CHECK(pwrite(fd, garbage.data(), garbage.size(), good_size) ==
          static_cast<ssize_t>(garbage.size()));
    close(fd);
  }

  {
    WALManager wal(path);  // truncates the torn half-record on open
    CHECK(wal.ValidRecords().size() == 1);
    Page p2 = MakePage(2, "after recovery");
    wal.AppendRecord(2, &p2);
    CHECK(wal.ValidRecords().size() == 2);
  }

  {
    WALManager wal(path);  // fully fresh scan from disk
    CHECK(wal.ValidRecords().size() == 2);
    CHECK(wal.ValidRecords()[1].page_id == 2);
    CHECK(MarkerMatches(wal.ValidRecords()[1].page, "after recovery"));
  }

  RemoveIfExists(path);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"AppendAndReopen", TestAppendAndReopen},
      {"TornTailIsTruncatedOnOpen", TestTornTailIsTruncatedOnOpen},
      {"AppendAfterTruncationIsReachable",
       TestAppendAfterTruncationIsReachable},
  };

  int failures = 0;
  for (auto& [name, fn] : tests) {
    try {
      fn();
      std::printf("[PASS] %s\n", name.c_str());
    } catch (const std::exception& e) {
      std::printf("[FAIL] %s: %s\n", name.c_str(), e.what());
      failures++;
    }
  }

  std::printf("\n%zu tests, %d failed\n", tests.size(), failures);
  return failures == 0 ? 0 : 1;
}