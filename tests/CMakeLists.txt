add_executable(test_disk_manager test_disk_manager.cpp)
target_link_libraries(test_disk_manager PRIVATE reldb_core)
add_test(NAME test_disk_manager COMMAND test_disk_manager)

add_executable(test_buffer_pool test_buffer_pool.cpp)
target_link_libraries(test_buffer_pool PRIVATE reldb_core)
add_test(NAME test_buffer_pool COMMAND test_buffer_pool)

add_executable(test_btree test_btree.cpp)
target_link_libraries(test_btree PRIVATE reldb_core)
add_test(NAME test_btree COMMAND test_btree)