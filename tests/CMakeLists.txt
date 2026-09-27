# Native tests, built with -DBUILD_TESTS=ON (see tests/run_tests.sh)
project(tests LANGUAGES CXX)

include_directories(${CMAKE_SOURCE_DIR}/tools ${CMAKE_SOURCE_DIR}/client ${CMAKE_SOURCE_DIR}/do_lib)

# Production protocol code: server from do_lib, client from the JNI library
add_executable(flash_ipc_test
    flash_ipc_test.cpp
    ${CMAKE_SOURCE_DIR}/do_lib/ipc.cpp
    ${CMAKE_SOURCE_DIR}/client/flash_ipc_client.cpp)
target_link_libraries(flash_ipc_test pthread)

add_executable(proc_util_test
    proc_util_test.cpp
    ${CMAKE_SOURCE_DIR}/client/proc_util.cpp)
target_link_libraries(proc_util_test pthread)

add_executable(sock_ipc_test
    sock_ipc_test.cpp
    ${CMAKE_SOURCE_DIR}/client/sock_ipc.cpp)
target_compile_definitions(sock_ipc_test PRIVATE TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
