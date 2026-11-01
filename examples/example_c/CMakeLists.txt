# examples/example_c/CMakeLists.txt
# C 示例：服务端 + 客户端

set(APP_THREAD_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../common/app_thread.cpp)
set(APP_THREAD_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../common)

add_executable(example_c_sensor_server sensor_server.c ${APP_THREAD_SOURCE})
target_include_directories(example_c_sensor_server PRIVATE ${APP_THREAD_INCLUDE})
target_link_libraries(example_c_sensor_server PRIVATE omnibinder_static Threads::Threads)

add_executable(example_c_sensor_client sensor_client.c ${APP_THREAD_SOURCE})
target_include_directories(example_c_sensor_client PRIVATE ${APP_THREAD_INCLUDE})
target_link_libraries(example_c_sensor_client PRIVATE omnibinder_static Threads::Threads)

# IDL 代码生成（C 语言），一次生成并复用到两个目标
omnic_generate(
    TARGET example_c_sensor_server example_c_sensor_client
    LANGUAGE c
    FILES
        ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
        ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
)
