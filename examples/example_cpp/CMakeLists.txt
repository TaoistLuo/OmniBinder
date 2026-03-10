# examples/example_cpp/CMakeLists.txt
# C++ 示例：服务端 + 客户端

add_executable(example_cpp_sensor_server sensor_server.cpp)
target_link_libraries(example_cpp_sensor_server PRIVATE omnibinder_static)

add_executable(example_cpp_sensor_client sensor_client.cpp)
target_link_libraries(example_cpp_sensor_client PRIVATE omnibinder_static)

# IDL 代码生成（C++）
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_cpp_sensor_server
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    omnic_generate(
        TARGET example_cpp_sensor_client
        LANGUAGE cpp
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    if(TARGET omni-idlc)
        add_dependencies(example_cpp_sensor_server omni-idlc)
        add_dependencies(example_cpp_sensor_client omni-idlc)
    else()
        add_dependencies(example_cpp_sensor_server omnic)
        add_dependencies(example_cpp_sensor_client omnic)
    endif()
endif()
