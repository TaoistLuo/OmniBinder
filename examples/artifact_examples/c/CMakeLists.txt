cmake_minimum_required(VERSION 3.10)
project(OmniBinderCurrentExamples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(OmniBinder REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(EXAMPLE_BIDL_FILES
    ${CMAKE_CURRENT_LIST_DIR}/../../common_types.bidl
    ${CMAKE_CURRENT_LIST_DIR}/../../sensor_service.bidl
)

add_executable(example_c_sensor_server
    ${CMAKE_CURRENT_LIST_DIR}/../../example_c/sensor_server.c)
target_link_libraries(example_c_sensor_server PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET example_c_sensor_server
    LANGUAGE c
    FILES ${EXAMPLE_BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/c
)

add_executable(example_c_sensor_client
    ${CMAKE_CURRENT_LIST_DIR}/../../example_c/sensor_client.c)
target_link_libraries(example_c_sensor_client PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET example_c_sensor_client
    LANGUAGE c
    FILES ${EXAMPLE_BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/c
)
