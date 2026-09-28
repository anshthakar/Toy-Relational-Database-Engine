cmake_minimum_required(VERSION 3.16)
project(reldb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# debug builds get sanitizers on by default
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
        src/token.cpp
        src/lexer.cpp
        src/ast.cpp
        src/parser.cpp
)

enable_testing()
add_subdirectory(tests)