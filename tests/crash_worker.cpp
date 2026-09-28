
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

    std::fflush(nullptr);
    return 1;
}