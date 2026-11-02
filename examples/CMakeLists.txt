# examples/CMakeLists.txt

# 示例可执行文件输出到 target/example
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/example)

# 完整类型/能力矩阵示例
add_subdirectory(all_type_example/example_cpp)
add_subdirectory(all_type_example/example_c)

# 最小快速开始示例（目录内只保留 4 个源文件 + 2 个 IDL）
set(SIMPLE_EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/simple_example)
set(APP_THREAD_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/common/app_thread.cpp)
set(APP_THREAD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/common)

add_executable(simple_cpp_server ${SIMPLE_EXAMPLE_DIR}/cpp_server.cpp)
target_link_libraries(simple_cpp_server PRIVATE omnibinder_static)

add_executable(simple_cpp_client ${SIMPLE_EXAMPLE_DIR}/cpp_client.cpp)
target_link_libraries(simple_cpp_client PRIVATE omnibinder_static)

omnic_generate(
    TARGET simple_cpp_server simple_cpp_client
    LANGUAGE cpp
    FILES
        ${SIMPLE_EXAMPLE_DIR}/common.bidl
        ${SIMPLE_EXAMPLE_DIR}/example.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/simple_generated/cpp
)

add_executable(simple_c_server ${SIMPLE_EXAMPLE_DIR}/c_server.c ${APP_THREAD_SOURCE})
target_include_directories(simple_c_server PRIVATE ${APP_THREAD_INCLUDE})
target_link_libraries(simple_c_server PRIVATE omnibinder_static Threads::Threads)

add_executable(simple_c_client ${SIMPLE_EXAMPLE_DIR}/c_client.c ${APP_THREAD_SOURCE})
target_include_directories(simple_c_client PRIVATE ${APP_THREAD_INCLUDE})
target_link_libraries(simple_c_client PRIVATE omnibinder_static Threads::Threads)

omnic_generate(
    TARGET simple_c_server simple_c_client
    LANGUAGE c
    FILES
        ${SIMPLE_EXAMPLE_DIR}/common.bidl
        ${SIMPLE_EXAMPLE_DIR}/example.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/simple_generated/c
)
