#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "buffer_pool.h"
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
  return "/tmp/reldb_bp_test_" + name + ".db";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

void WriteMarker(Page* page, const char* msg) {
  std::memcpy(page->data() + sizeof(PageHeader), msg, std::strlen(msg) + 1);
}

bool MarkerMatches(const Page* page, const char* msg) {
  return std::strcmp(reinterpret_cast<const char*>(page->data() +
                                                     sizeof(PageHeader)),
                      msg) == 0;
}

void TestNewPageRoundTrip() {
  std::string path = TempDbPath("roundtrip");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(4, &dm);

  page_id_t id;
  {
    PageGuard guard = bp.NewPage();
    CHECK(guard.valid());
    id = guard.page_id();
    WriteMarker(guard.page(), "first page");
    guard.MarkDirty();
  }  // guard destructs here -> unpinned, but still cached (pool not full)

  {
    PageGuard guard = bp.FetchPage(id);
    CHECK(guard.valid());
    CHECK(MarkerMatches(guard.page(), "first page"));
  }

  RemoveIfExists(path);
}

void TestPinPreventsEviction() {
  std::string path = TempDbPath("pin");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(2, &dm);  // only 2 frames

  PageGuard g1 = bp.NewPage();
  PageGuard g2 = bp.NewPage();
  CHECK(g1.valid());
  CHECK(g2.valid());
  // Both frames are full and both guards are still alive (pinned) — no
  // frame is evictable, so a third page request must fail rather than
  // silently stomping on a page still in use.
  PageGuard g3 = bp.NewPage();
  CHECK(!g3.valid());

  RemoveIfExists(path);
}

void TestLRUEvictsLeastRecentlyUsed() {
  std::string path = TempDbPath("lru");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(2, &dm);  // only 2 frames — forces eviction on the 3rd page

  page_id_t id_a, id_b, id_c;
  {
    PageGuard ga = bp.NewPage();
    id_a = ga.page_id();
    WriteMarker(ga.page(), "page A");
    ga.MarkDirty();
  }  // A unpinned, now the LRU tail (only page touched so far)

  {
    PageGuard gb = bp.NewPage();
    id_b = gb.page_id();
    WriteMarker(gb.page(), "page B");
    gb.MarkDirty();
  }  // B unpinned; LRU order (front->back) is now B, A

  // Touch A again so B becomes the least-recently-used one.
  {
    PageGuard ga2 = bp.FetchPage(id_a);
    CHECK(MarkerMatches(ga2.page(), "page A"));
  }  // LRU order is now A, B — B is the tail

  {
    // This should evict B (the tail), not A.
    PageGuard gc = bp.NewPage();
    id_c = gc.page_id();
    WriteMarker(gc.page(), "page C");
    gc.MarkDirty();
  }

  // A should still be cheaply fetchable and correct (was never evicted).
  {
    PageGuard ga3 = bp.FetchPage(id_a);
    CHECK(MarkerMatches(ga3.page(), "page A"));
  }
  // B was evicted, but its dirty content must have been flushed to disk on
  // eviction — fetching it now re-reads from disk and should still be
  // correct, not lost or garbage.
  {
    PageGuard gb2 = bp.FetchPage(id_b);
    CHECK(MarkerMatches(gb2.page(), "page B"));
  }
  {
    PageGuard gc2 = bp.FetchPage(id_c);
    CHECK(MarkerMatches(gc2.page(), "page C"));
  }

  RemoveIfExists(path);
}

void TestDirtyPageSurvivesRestart() {
  std::string path = TempDbPath("restart");
  RemoveIfExists(path);

  page_id_t id;
  {
    DiskManager dm(path);
    BufferPool bp(1, &dm);  // single frame — the second NewPage forces
                             // an eviction (and flush) of the first
    {
      PageGuard g1 = bp.NewPage();
      id = g1.page_id();
      WriteMarker(g1.page(), "durable data");
      g1.MarkDirty();
    }
    // Force the only frame to be reused, which evicts page `id` and, since
    // it's dirty, must flush it to disk before reuse.
    { PageGuard g2 = bp.NewPage(); }
  }  // DiskManager + BufferPool destruct — simulates process exit

  // Fresh instances, nothing cached in memory: this can only pass if the
  // eviction above actually wrote the page to disk.
  {
    DiskManager dm(path);
    BufferPool bp(4, &dm);
    PageGuard guard = bp.FetchPage(id);
    CHECK(guard.valid());
    CHECK(MarkerMatches(guard.page(), "durable data"));
  }

  RemoveIfExists(path);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"NewPageRoundTrip", TestNewPageRoundTrip},
      {"PinPreventsEviction", TestPinPreventsEviction},
      {"LRUEvictsLeastRecentlyUsed", TestLRUEvictsLeastRecentlyUsed},
      {"DirtyPageSurvivesRestart", TestDirtyPageSurvivesRestart},
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