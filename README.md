# Toy Relational Database Engine

A relational database engine built from scratch in C++.

## Built with

- C++
- CMake
- AddressSanitizer / UndefinedBehaviorSanitizer (Debug builds)
- On-disk B+tree with a buffer pool (LRU eviction, RAII page pinning)
- Write-ahead log with crash recovery
- Hand-written recursive-descent SQL parser (no parser-generator)
- Multi-table catalog with a cost-based query planner (nested-loop +
  hash join, basic join-order selection)

## Building

Linux or WSL2 required (`DiskManager` uses `open`/`pread`/`pwrite`/`fsync`
directly — no native Windows/MSVC/MinGW support).

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```
