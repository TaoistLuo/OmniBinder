cmake_minimum_required(VERSION 3.10)
project(OmniBinderSensorHMIExamples LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT OmniBinder_DIR)
    set(OmniBinder_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/install/lib/cmake/OmniBinder"
        CACHE PATH "Path to OmniBinder package config")
endif()

find_package(OmniBinder REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(BIDL_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/idl/sensor_hmi_common.bidl
    ${CMAKE_CURRENT_SOURCE_DIR}/idl/sensor_hmi_service.bidl
)

add_executable(sensor_cpp sensor_cpp/sensor_service.cpp)
target_link_libraries(sensor_cpp PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET sensor_cpp
    LANGUAGE cpp
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)

add_executable(hmi_cpp hmi_cpp/hmi_client.cpp)
target_link_libraries(hmi_cpp PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET hmi_cpp
    LANGUAGE cpp
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cpp
)

add_executable(sensor_c sensor_c/sensor_service.c)
target_link_libraries(sensor_c PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET sensor_c
    LANGUAGE c
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/c
)

add_executable(hmi_c hmi_c/hmi_client.c)
target_link_libraries(hmi_c PRIVATE OmniBinder::omnibinder_static)
omnic_generate(
    TARGET hmi_c
    LANGUAGE c
    FILES ${BIDL_FILES}
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/c
)
