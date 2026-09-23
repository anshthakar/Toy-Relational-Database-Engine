add_executable(test_disk_manager test_disk_manager.cpp)
target_link_libraries(test_disk_manager PRIVATE reldb_core)
add_test(NAME test_disk_manager COMMAND test_disk_manager)

add_executable(test_buffer_pool test_buffer_pool.cpp)
target_link_libraries(test_buffer_pool PRIVATE reldb_core)
add_test(NAME test_buffer_pool COMMAND test_buffer_pool)

add_executable(test_btree test_btree.cpp)
target_link_libraries(test_btree PRIVATE reldb_core)
add_test(NAME test_btree COMMAND test_btree)

add_executable(test_wal test_wal.cpp)
target_link_libraries(test_wal PRIVATE reldb_core)
add_test(NAME test_wal COMMAND test_wal)

add_executable(test_recovery test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE reldb_core)
add_test(NAME test_recovery COMMAND test_recovery)

# crash_worker is deliberately its own standalone binary, not a test — it
# gets fork/exec'd by test_crash_kill and SIGKILLs itself on purpose. It
# is not added via add_test(); nobody runs it directly.
add_executable(crash_worker crash_worker.cpp)
target_link_libraries(crash_worker PRIVATE reldb_core)

add_executable(test_crash_kill test_crash_kill.cpp)
target_link_libraries(test_crash_kill PRIVATE reldb_core)
# Bakes the just-built worker's absolute path into test_crash_kill, so the
# test doesn't have to guess a relative path that would break depending on
# where ctest/the binary is invoked from.
target_compile_definitions(test_crash_kill PRIVATE
        CRASH_WORKER_PATH="$<TARGET_FILE:crash_worker>")
add_dependencies(test_crash_kill crash_worker)
add_test(NAME test_crash_kill COMMAND test_crash_kill)