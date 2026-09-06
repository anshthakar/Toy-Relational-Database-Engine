#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "disk_manager.h"
#include "page.h"

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

std::string TempDbPath(const std::string& name) {
  return "/tmp/reldb_test_" + name + ".db";
}

void RemoveIfExists(const std::string& path) {
  unlink(path.c_str());
}

void TestWriteReadRoundTrip() {
  std::string path = TempDbPath("roundtrip");
  RemoveIfExists(path);
  DiskManager dm(path);

  Page page;
  page.header()->page_id = 0;
  page.header()->page_type = PageType::LEAF;
  page.header()->num_slots = 3;
  page.header()->free_space_ptr = 1234;
  page.header()->lsn = 42;

  const char* msg = "hello reldb";
  std::memcpy(page.data() + sizeof(PageHeader), msg, std::strlen(msg));

  page_id_t id = dm.AllocatePage();
  CHECK(id == 0);
  dm.WritePage(id, &page);

  Page read_back;
  dm.ReadPage(id, &read_back);

  CHECK(read_back.header()->page_id == 0);
  CHECK(read_back.header()->page_type == PageType::LEAF);
  CHECK(read_back.header()->num_slots == 3);
  CHECK(read_back.header()->free_space_ptr == 1234);
  CHECK(read_back.header()->lsn == 42);
  CHECK(std::memcmp(read_back.data() + sizeof(PageHeader), msg,
                     std::strlen(msg)) == 0);

  RemoveIfExists(path);
}

void TestAllocatePageIdsAreUniqueAndSequential() {
  std::string path = TempDbPath("alloc");
  RemoveIfExists(path);
  DiskManager dm(path);

  std::vector<page_id_t> ids;
  for (int i = 0; i < 10; ++i) {
    ids.push_back(dm.AllocatePage());
  }
  for (size_t i = 0; i < ids.size(); ++i) {
    CHECK(ids[i] == i);
  }
  CHECK(dm.NumPages() == 10);

  RemoveIfExists(path);
}

void TestChecksumCatchesCorruption() {
  std::string path = TempDbPath("corrupt");
  RemoveIfExists(path);

  {
    DiskManager dm(path);
    Page page;
    page.header()->page_id = 0;
    page.header()->page_type = PageType::LEAF;
    page_id_t id = dm.AllocatePage();
    dm.WritePage(id, &page);
    dm.Flush();
  }

  int fd = open(path.c_str(), O_RDWR);
  CHECK(fd >= 0);
  char garbage = 0x7F;
  CHECK(pwrite(fd, &garbage, 1, 100) == 1);
  close(fd);

  DiskManager dm2(path);
  Page page;
  bool threw = false;
  try {
    dm2.ReadPage(0, &page);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);

  RemoveIfExists(path);
}

void TestPersistsAcrossReopen() {
  std::string path = TempDbPath("persist");
  RemoveIfExists(path);

  {
    DiskManager dm(path);
    Page page;
    page.header()->page_id = 0;
    page.header()->lsn = 99;
    page_id_t id = dm.AllocatePage();
    dm.WritePage(id, &page);
    dm.Flush();
  }

  {
    DiskManager dm(path);
    CHECK(dm.NumPages() == 1);
    Page page;
    dm.ReadPage(0, &page);
    CHECK(page.header()->lsn == 99);
  }

  RemoveIfExists(path);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"WriteReadRoundTrip", TestWriteReadRoundTrip},
      {"AllocatePageIdsAreUniqueAndSequential",
       TestAllocatePageIdsAreUniqueAndSequential},
      {"ChecksumCatchesCorruption", TestChecksumCatchesCorruption},
      {"PersistsAcrossReopen", TestPersistsAcrossReopen},
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