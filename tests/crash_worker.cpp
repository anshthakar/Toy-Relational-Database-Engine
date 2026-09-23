// Standalone worker process for the real-process-kill crash test
// (test_crash_kill.cpp). Deliberately NOT part of reldb_core or any test
// executable's normal flow — this binary's whole job is to die abruptly,
// on purpose, with zero cleanup, so the harness can prove recovery works
// against whatever the OS actually made durable before that, not against
// our own in-process model of what "durable" should mean.
//
// argv[1] = data file path
// argv[2] = wal file path
// argv[3] = number of complete Insert() calls to perform before killing
//           itself
//
// After the inserts, it calls raise(SIGKILL) on itself: no destructors
// run, no fds get explicitly closed, nothing further is written. Any
// data recoverable after this genuinely only got there via the WAL's own
// fsync — there's no cooperative shutdown path to fall back on.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "database.h"

using namespace reldb;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                      "usage: crash_worker <data_path> <wal_path> <n_inserts>\n");
        return 2;
    }

    std::string data_path = argv[1];
    std::string wal_path = argv[2];
    int n = std::atoi(argv[3]);

    Database db(data_path, wal_path, /*pool_size=*/8);
    for (int i = 0; i < n; ++i) {
        db.tree().Insert(i, "val" + std::to_string(i));
    }

    std::fflush(nullptr);  // flush stdio buffers only — irrelevant to DB
    // durability (that's WAL's fsync, already done
    // synchronously inside each Insert() above),
    // just keeps this process's own diagnostic
    // output honest if anyone redirects it.

    raise(SIGKILL);  // no return from here on a normal system

    // Unreachable in practice; present only so the compiler doesn't warn
    // about a signal-terminated function falling off the end.
    return 1;
}