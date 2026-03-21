cmake_minimum_required(VERSION 3.10)
project(OmniBinderCurrentExamples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 指定 CMAKE_PREFIX_PATH

find_package(OmniBinder REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(EXAMPLE_BIDL_FILES
    ${CMAKE_CURRENT_LIST_DIR}/../../common_types.bidl
    ${CMAKE_CURRENT_LIST_DIR}/../../sensor_service.bidl
)

add_executable(example_cpp_sensor_server
    ${CMAKE_CURRENT_LIST_DIR}/../../example_cpp/sensor_server.cpp)
target_link_libraries(example_cpp_sensor_server PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET example_cpp_sensor_server
    LANGUAGE cpp
    FILES ${EXAMPLE_BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)

add_executable(example_cpp_sensor_client
    ${CMAKE_CURRENT_LIST_DIR}/../../example_cpp/sensor_client.cpp)
target_link_libraries(example_cpp_sensor_client PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET example_cpp_sensor_client
    LANGUAGE cpp
    FILES ${EXAMPLE_BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)