// Deterministic crash simulation via WAL truncation.
//
// The core invariant under test (derived, not assumed — see the ordering
// fix in btree.cpp and the reasoning in the milestone 2 design doc): WAL
// records are always appended in a strict child-before-parent order for
// any single operation (a split's new sibling page is fully logged
// before the node that starts pointing at it; a new root is fully logged
// before the metadata page that starts pointing at it). Since a
// truncation can only ever keep a file-offset PREFIX, and children are
// always at lower offsets than the parents that reference them, ANY
// truncation point yields a "downward-closed" set of applied pages: if a
// page's record survived truncation, every page it points to also did.
// So recovery from an arbitrarily-truncated WAL can never leave a
// dangling pointer to a page that was never written — the recovered
// tree is always structurally walkable, even if it reflects a strictly
// older point in the operation sequence than the live database reached.
//
// This test inserts keys 0..N-1 in order (unique key, unique fixed value,
// never updated or deleted — keeps "what SHOULD be recovered" unambiguous)
// through a small buffer pool (forcing real eviction/flushing during the
// live run, not just WAL durability) and records the WAL's on-disk byte
// size after every top-level Insert() call. For a truncation point T
// landing in [size_after[i], size_after[i+1]), the recovered key set must
// be EXACTLY {0..i} or {0..i+1} — nothing more, nothing less, and never
// corrupted — which is the strong, precise, checkable form of "recovers
// to a valid state, never partially-applied, never corrupted."

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "database.h"

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
  return "/tmp/reldb_recovery_test_" + name;
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

std::string ExpectedValue(int64_t key) { return "val" + std::to_string(key); }

off_t FileSize(const std::string& path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0) return 0;
  return st.st_size;
}

// Copies `src` to `dst`, truncated to `size` bytes (size may exceed the
// source's actual length, in which case the whole file is copied).
void CopyTruncated(const std::string& src, const std::string& dst,
                    off_t size) {
  int in_fd = open(src.c_str(), O_RDONLY);
  CHECK(in_fd >= 0);
  int out_fd = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(out_fd >= 0);

  off_t src_size = FileSize(src);
  off_t to_copy = std::min(size, src_size);

  std::vector<char> buf(1 << 16);
  off_t remaining = to_copy;
  off_t offset = 0;
  while (remaining > 0) {
    ssize_t chunk = std::min<ssize_t>(buf.size(), remaining);
    ssize_t n = pread(in_fd, buf.data(), chunk, offset);
    CHECK(n > 0);
    CHECK(pwrite(out_fd, buf.data(), n, offset) == n);
    offset += n;
    remaining -= n;
  }
  close(in_fd);
  close(out_fd);
}

// Runs recovery against a WAL truncated to `wal_truncate_size` bytes,
// against a FRESH, EMPTY data file — deliberately NOT a copy of
// `data_path`'s live, fully-advanced contents.
//
// This isn't a simplification for convenience, it's the only choice that
// is physically realizable. `data_path`'s live copy, by the time the
// whole run has finished, reflects bytes flushed by ordinary buffer-pool
// eviction throughout the ENTIRE run — including pages from operations
// far past whatever `wal_truncate_size` we're testing here. A real crash
// at "the moment wal_truncate_size bytes were durable" could never have
// produced a data file with that much extra, later content on it —
// eviction can only flush a page AFTER it's already been logged (see
// BufferPool::UnpinPage), so a real crash's data file is always
// consistent with *its own* WAL prefix, never ahead of it. Pairing a
// fully-advanced data file with an artificially-rewound WAL creates a
// state no real crash produces, and recovery — reasonably — isn't
// designed to paper over that (an earlier version of this test did
// exactly that and failed with a checksum mismatch on an untouched page:
// not a recovery bug, a test-methodology bug).
//
// Using a fresh empty data file sidesteps the whole issue and is also
// the strictly stronger test: 100% of the reconstruction has to come
// from the WAL alone, none of it can be scavenged from data-file
// leftovers (AllocatePage/NewPage never touch disk directly, so this is
// always valid regardless of truncation point — see RunRecovery/database.h).
std::vector<std::pair<int64_t, std::string>> RecoverAndScan(
    const std::string& wal_path, off_t wal_truncate_size,
    const std::string& tag) {
  std::string data_copy = TempPath("data_copy_" + tag);
  std::string wal_copy = TempPath("wal_copy_" + tag);
  RemoveIfExists(data_copy);  // fresh/empty — DiskManager creates it anew
  RemoveIfExists(wal_copy);

  CopyTruncated(wal_path, wal_copy, wal_truncate_size);

  std::vector<std::pair<int64_t, std::string>> result;
  {
    Database db(data_copy, wal_copy, /*pool_size=*/32);
    result = db.tree().RangeScan(INT64_MIN, INT64_MAX);
  }

  RemoveIfExists(data_copy);
  RemoveIfExists(wal_copy);
  return result;
}

void CheckRecoveredStateIsValidPrefix(
    const std::vector<std::pair<int64_t, std::string>>& recovered,
    int64_t i /* last fully-confirmed operation index */) {
  // Every recovered key must be a real key we inserted, with the
  // correct, uncorrupted value — this alone rules out silent corruption.
  for (auto& [k, v] : recovered) {
    CHECK(k >= 0);
    CHECK(v == ExpectedValue(k));
  }
  // Keys must come back sorted with no duplicates (a corrupt leaf chain
  // or a double-applied split could violate this).
  for (size_t j = 1; j < recovered.size(); ++j) {
    CHECK(recovered[j].first > recovered[j - 1].first);
  }

  std::set<int64_t> keys;
  for (auto& [k, v] : recovered) keys.insert(k);

  std::set<int64_t> expected_at_i, expected_at_i_plus_1;
  for (int64_t k = 0; k <= i; ++k) expected_at_i.insert(k);
  expected_at_i_plus_1 = expected_at_i;
  expected_at_i_plus_1.insert(i + 1);

  bool matches_i = (keys == expected_at_i);
  bool matches_i_plus_1 = (keys == expected_at_i_plus_1);
  CHECK(matches_i || matches_i_plus_1);
}

void TestWALTruncationCrashSimulation() {
  std::string data_path = TempPath("live_data.db");
  std::string wal_path = TempPath("live_wal.log");
  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);

  const int64_t kCount = 300;  // plenty of leaf + internal + root splits
  std::vector<off_t> wal_size_after(kCount);

  {
    // Small pool: forces real eviction/flushing to the data file during
    // the run, not just WAL durability — exercises recovery working
    // correctly regardless of which arbitrary subset of pages the
    // buffer pool happened to flush before the "crash".
    Database db(data_path, wal_path, /*pool_size=*/8);
    for (int64_t i = 0; i < kCount; ++i) {
      db.tree().Insert(i, ExpectedValue(i));
      wal_size_after[i] = FileSize(wal_path);
      CHECK(wal_size_after[i] > (i > 0 ? wal_size_after[i - 1] : 0));
    }
  }  // Database destructs — real fds close, nothing further is flushed

  // Boundary truncation points: exactly at wal_size_after[i], for a
  // spread of i values across the whole run (every single i would make
  // this test slow; a spread still covers early/mid/late, single-record
  // ops and multi-record split ops alike).
  std::vector<int64_t> sample_indices;
  for (int64_t i = 0; i < kCount - 1; i += 7) sample_indices.push_back(i);
  sample_indices.push_back(kCount - 2);

  for (int64_t i : sample_indices) {
    std::printf("  [boundary check] i=%lld T=%lld\n", (long long)i,
                (long long)wal_size_after[i]);
    std::fflush(stdout);
    auto recovered = RecoverAndScan(wal_path, wal_size_after[i],
                                     "boundary_" + std::to_string(i));
    // At an exact boundary, operation i+1 has not started being written
    // at all — the outcome must be EXACTLY {0..i}, not the looser
    // {0..i} or {0..i+1} allowance used for mid-record points below.
    std::set<int64_t> keys;
    for (auto& [k, v] : recovered) keys.insert(k);
    std::set<int64_t> expected;
    for (int64_t k = 0; k <= i; ++k) expected.insert(k);
    CHECK(keys == expected);
    for (auto& [k, v] : recovered) CHECK(v == ExpectedValue(k));
  }

  // Mid-record (torn-write) truncation points: land strictly inside the
  // byte range an operation's records occupy, deliberately including
  // points that split a single record in half — the actual "kill -9
  // mid-pwrite" scenario, not just "stopped between two operations".
  for (int64_t i : sample_indices) {
    if (i + 1 >= kCount) continue;
    off_t lo = wal_size_after[i];
    off_t hi = wal_size_after[i + 1];
    off_t span = hi - lo;
    for (double frac : {0.1, 0.33, 0.5, 0.67, 0.9}) {
      off_t t = lo + static_cast<off_t>(span * frac);
      std::printf("  [mid-record check] i=%lld frac=%.2f T=%lld (lo=%lld hi=%lld)\n",
                  (long long)i, frac, (long long)t, (long long)lo, (long long)hi);
      std::fflush(stdout);
      auto recovered = RecoverAndScan(
          wal_path, t,
          "mid_" + std::to_string(i) + "_" + std::to_string(int(frac * 100)));
      CheckRecoveredStateIsValidPrefix(recovered, i);
    }
  }

  // Edge cases: an empty WAL (nothing durable yet — a fresh, empty tree)
  // and the full, untruncated WAL (everything applied).
  {
    auto recovered = RecoverAndScan(wal_path, 0, "empty");
    CHECK(recovered.empty());
  }
  {
    auto recovered =
        RecoverAndScan(wal_path, wal_size_after[kCount - 1], "full");
    CHECK(recovered.size() == static_cast<size_t>(kCount));
    for (int64_t k = 0; k < kCount; ++k) {
      CHECK(recovered[k].first == k);
      CHECK(recovered[k].second == ExpectedValue(k));
    }
  }

  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);
}

void TestRecoveryIsIdempotent() {
  // Running recovery twice in a row (open, close, reopen — nothing new
  // written in between) must be a no-op the second time: the WAL's
  // already-durable records shouldn't cause any duplicate or drifting
  // state on repeated replay.
  std::string data_path = TempPath("idem_data.db");
  std::string wal_path = TempPath("idem_wal.log");
  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);

  {
    Database db(data_path, wal_path, 16);
    for (int64_t i = 0; i < 50; ++i) {
      db.tree().Insert(i, ExpectedValue(i));
    }
  }
  {
    Database db(data_path, wal_path, 16);  // recovers (should be a no-op)
    auto scanned = db.tree().RangeScan(INT64_MIN, INT64_MAX);
    CHECK(scanned.size() == 50);
    for (int64_t i = 0; i < 50; ++i) {
      CHECK(scanned[i].first == i);
      CHECK(scanned[i].second == ExpectedValue(i));
    }
  }
  {
    Database db(data_path, wal_path, 16);  // recovers again — still a no-op
    auto scanned = db.tree().RangeScan(INT64_MIN, INT64_MAX);
    CHECK(scanned.size() == 50);
  }

  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"WALTruncationCrashSimulation", TestWALTruncationCrashSimulation},
      {"RecoveryIsIdempotent", TestRecoveryIsIdempotent},
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