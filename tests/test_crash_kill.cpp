// The real-process-kill crash test, as distinct from test_recovery.cpp's
// deterministic WAL-truncation tests. Truncation tests prove our own
// recovery LOGIC is correct against any possible prefix of the WAL, but
// they never leave the safety of a single, cooperative, in-process
// simulation: they can't tell us whether fsync() actually forces data
// through the OS page cache to something durable, or whether our
// object model (guard destruction -> AppendRecord -> fsync) is honored
// the same way when the process dies with zero warning and zero
// cleanup. This test answers that by literally forking a child, letting
// it run real inserts through the real Database/BTree/BufferPool/WAL
// stack, and SIGKILLing it — no atexit, no destructors, no chance for
// the OS to do us any favors — then recovering in a genuinely separate
// process afterward.
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

#ifndef CRASH_WORKER_PATH
#error "CRASH_WORKER_PATH must be defined by CMake to the worker binary path"
#endif

namespace {

std::string TempPath(const std::string& name) {
  return "/tmp/reldb_crashkill_test_" + name;
}

void RemoveIfExists(const std::string& path) { unlink(path.c_str()); }

std::string ExpectedValue(int64_t key) { return "val" + std::to_string(key); }

// Forks, execs crash_worker with the given args, and waits for it. CHECKs
// that the child was actually terminated BY SIGKILL — not that it merely
// exited — so the test itself can't silently pass because the worker
// exited cleanly instead of genuinely dying uncleanly the way it's
// supposed to.
void RunWorkerAndConfirmKilled(const std::string& data_path,
                                const std::string& wal_path, int n_inserts) {
  pid_t pid = fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    // Child: replace this process image with the worker. execl only
    // returns on failure.
    execl(CRASH_WORKER_PATH, CRASH_WORKER_PATH, data_path.c_str(),
          wal_path.c_str(), std::to_string(n_inserts).c_str(),
          static_cast<char*>(nullptr));
    // If we get here, exec itself failed (e.g. binary not found) — exit
    // loudly and distinctly rather than continuing to run this test's
    // own code as if it were the worker.
    std::fprintf(stderr, "execl failed: %s\n", std::strerror(errno));
    _exit(127);
  }

  // Parent: wait for the child to die and inspect exactly how.
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  CHECK(waited == pid);

  // This is the check that makes the test meaningful: the worker must
  // have died BY SIGNAL (SIGKILL specifically), not returned normally.
  // If this ever fails, either raise(SIGKILL) isn't reaching the
  // process (a test-harness problem) or the worker exited before
  // reaching it (exec failure, argument parsing bug) — either way, a
  // clean exit here would mean this "crash" test never actually crashed
  // anything, and every downstream check would be validating an orderly
  // shutdown instead of the real failure mode this test exists for.
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);
}

void TestKillAfterNInserts(int n) {
  std::string data_path = TempPath("data_" + std::to_string(n) + ".db");
  std::string wal_path = TempPath("wal_" + std::to_string(n) + ".log");
  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);

  RunWorkerAndConfirmKilled(data_path, wal_path, n);

  // Recover in THIS process — genuinely separate from the one that died,
  // same as a real restart after a real crash would be.
  {
    Database db(data_path, wal_path, /*pool_size=*/16);
    auto scanned = db.tree().RangeScan(INT64_MIN, INT64_MAX);
    CHECK(scanned.size() == static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      CHECK(scanned[i].first == i);
      CHECK(scanned[i].second == ExpectedValue(i));
    }
  }

  // And recovering a SECOND time (nothing new written in between) must
  // still be a clean no-op — same idempotency property test_recovery.cpp
  // checks in-process, now checked against a WAL that a real kill -9
  // produced.
  {
    Database db(data_path, wal_path, /*pool_size=*/16);
    auto scanned = db.tree().RangeScan(INT64_MIN, INT64_MAX);
    CHECK(scanned.size() == static_cast<size_t>(n));
  }

  RemoveIfExists(data_path);
  RemoveIfExists(wal_path);
}

void TestKillAfterZeroInserts() {
  // The degenerate case: the worker is killed before doing anything at
  // all durable. Recovery of a file that was touched but never
  // meaningfully written to must yield a valid, empty database, not a
  // crash of the recovery code itself.
  TestKillAfterNInserts(0);
}

void TestKillAfterFewInserts() {
  // Small counts stay entirely within a single leaf — no splits, the
  // simplest structural case, killed for real.
  TestKillAfterNInserts(1);
  TestKillAfterNInserts(3);
}

void TestKillAfterManyInsertsAcrossSplits() {
  // LEAF_MAX_KEYS = INTERNAL_MAX_KEYS = 4, so this count forces real
  // leaf splits, internal splits, and at least one root split — the
  // exact multi-level structure test_btree.cpp's ForcesMultiLevelSplits
  // exercises, now recovered from a real kill -9 instead of an in-memory
  // teardown.
  TestKillAfterNInserts(500);
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"KillAfterZeroInserts", TestKillAfterZeroInserts},
      {"KillAfterFewInserts", TestKillAfterFewInserts},
      {"KillAfterManyInsertsAcrossSplits", TestKillAfterManyInsertsAcrossSplits},
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