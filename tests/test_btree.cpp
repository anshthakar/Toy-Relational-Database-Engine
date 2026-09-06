#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include "btree.h"
#include "buffer_pool.h"
#include "disk_manager.h"

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
  return "/tmp/reldb_btree_test_" + name + ".db";
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

std::string RandomValue(std::mt19937& rng, int64_t key) {
  // Embed the key in the value so a mismatch is easy to spot in a failure
  // message, padded with a few random chars so values aren't uniform
  // length (exercises value_len handling, not just fixed 8-byte payloads).
  std::uniform_int_distribution<int> len_dist(0, 10);
  std::string v = "v" + std::to_string(key) + "_";
  int extra = len_dist(rng);
  static const char charset[] = "abcdefghijklmnopqrstuvwxyz";
  for (int i = 0; i < extra; ++i) {
    v += charset[rng() % 26];
  }
  return v;
}

void TestBasicSmoke() {
  std::string path = TempDbPath("smoke");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  BTree tree(&bp);

  CHECK(!tree.Get(42).has_value());
  tree.Insert(42, "hello");
  CHECK(tree.Get(42).value() == "hello");

  tree.Insert(42, "updated");  // upsert
  CHECK(tree.Get(42).value() == "updated");

  CHECK(tree.Delete(42));
  CHECK(!tree.Get(42).has_value());
  CHECK(!tree.Delete(42));  // already gone

  RemoveIfExists(path);
}

void TestForcesMultiLevelSplits() {
  // LEAF_MAX_KEYS = INTERNAL_MAX_KEYS = 4, so a few hundred keys is
  // guaranteed to force leaf splits, internal splits, and at least one
  // root split (tree growing to 3+ levels) — not just leaf-level splits.
  std::string path = TempDbPath("splits");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(64, &dm);
  BTree tree(&bp);

  const int64_t kCount = 500;
  for (int64_t i = 0; i < kCount; ++i) {
    tree.Insert(i, "val" + std::to_string(i));
  }
  for (int64_t i = 0; i < kCount; ++i) {
    auto v = tree.Get(i);
    CHECK(v.has_value());
    CHECK(v.value() == "val" + std::to_string(i));
  }

  auto all = tree.RangeScan(0, kCount - 1);
  CHECK(all.size() == static_cast<size_t>(kCount));
  for (int64_t i = 0; i < kCount; ++i) {
    CHECK(all[i].first == i);  // range scan must come back sorted
  }

  RemoveIfExists(path);
}

void TestDifferentialFuzz() {
  std::string path = TempDbPath("fuzz");
  RemoveIfExists(path);
  DiskManager dm(path);
  BufferPool bp(128, &dm);
  BTree tree(&bp);

  std::map<int64_t, std::string> reference;
  std::mt19937 rng(1234);  // fixed seed: reproducible on failure
  std::uniform_int_distribution<int64_t> key_dist(0, 800);
  std::uniform_int_distribution<int> op_dist(0, 2);  // 0=insert 1=delete 2=get

  const int kOps = 5000;
  for (int op_num = 0; op_num < kOps; ++op_num) {
    int64_t key = key_dist(rng);
    int op = op_dist(rng);

    if (op == 0) {
      std::string value = RandomValue(rng, key);
      tree.Insert(key, value);
      reference[key] = value;
    } else if (op == 1) {
      bool tree_had = tree.Delete(key);
      bool ref_had = reference.erase(key) > 0;
      CHECK(tree_had == ref_had);
    } else {
      auto tree_val = tree.Get(key);
      auto ref_it = reference.find(key);
      bool ref_has = (ref_it != reference.end());
      CHECK(tree_val.has_value() == ref_has);
      if (ref_has) {
        CHECK(tree_val.value() == ref_it->second);
      }
    }

    // Periodically cross-check a full range scan against the reference —
    // catches ordering bugs and leaf-chain bugs that point queries alone
    // wouldn't surface.
    if (op_num % 500 == 499) {
      auto scanned = tree.RangeScan(INT64_MIN, INT64_MAX);
      CHECK(scanned.size() == reference.size());
      size_t i = 0;
      for (auto& [k, v] : reference) {
        CHECK(scanned[i].first == k);
        CHECK(scanned[i].second == v);
        i++;
      }
    }
  }

  // Final exhaustive check.
  auto scanned = tree.RangeScan(INT64_MIN, INT64_MAX);
  CHECK(scanned.size() == reference.size());
  size_t i = 0;
  for (auto& [k, v] : reference) {
    CHECK(scanned[i].first == k);
    CHECK(scanned[i].second == v);
    i++;
  }

  // Partial range scan check against a few random windows.
  std::uniform_int_distribution<int64_t> window_dist(0, 800);
  for (int t = 0; t < 20; ++t) {
    int64_t a = window_dist(rng);
    int64_t b = window_dist(rng);
    if (a > b) std::swap(a, b);
    auto partial = tree.RangeScan(a, b);
    auto ref_lo = reference.lower_bound(a);
    auto ref_hi = reference.upper_bound(b);
    size_t expected = std::distance(ref_lo, ref_hi);
    CHECK(partial.size() == expected);
    size_t j = 0;
    for (auto it = ref_lo; it != ref_hi; ++it, ++j) {
      CHECK(partial[j].first == it->first);
      CHECK(partial[j].second == it->second);
    }
  }

  RemoveIfExists(path);
}

void TestPersistsAcrossReopen() {
  std::string path = TempDbPath("persist");
  RemoveIfExists(path);

  {
    DiskManager dm(path);
    BufferPool bp(32, &dm);
    BTree tree(&bp);
    for (int64_t i = 0; i < 100; ++i) {
      tree.Insert(i, "p" + std::to_string(i));
    }
    bp.FlushAll();
  }  // simulate process exit

  {
    DiskManager dm(path);
    BufferPool bp(32, &dm);
    BTree tree(&bp);  // must read root_page_id from the metadata page
    for (int64_t i = 0; i < 100; ++i) {
      auto v = tree.Get(i);
      CHECK(v.has_value());
      CHECK(v.value() == "p" + std::to_string(i));
    }
  }

  RemoveIfExists(path);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"BasicSmoke", TestBasicSmoke},
      {"ForcesMultiLevelSplits", TestForcesMultiLevelSplits},
      {"DifferentialFuzz", TestDifferentialFuzz},
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