cmake_minimum_required(VERSION 3.16)
project(reldb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Debug builds get sanitizers on by default. This is not optional for a
# project graded on "undefined behavior is a real failure, not a style nit" —
# ASan/UBSan catch use-after-free, buffer overruns, and signed overflow that
# will otherwise hide silently until a crash test fails for a confusing reason.
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-fsanitize=address,undefined")

add_compile_options(-Wall -Wextra)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(reldb_core
        src/disk_manager.cpp
        src/buffer_pool.cpp
        src/btree.cpp
        src/wal.cpp
        src/recovery.cpp
        src/database.cpp
)

enable_testing()
add_subdirectory(tests)