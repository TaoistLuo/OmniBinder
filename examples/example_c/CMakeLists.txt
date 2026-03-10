# examples/example_c/CMakeLists.txt
# C 示例：服务端 + 客户端

add_executable(example_c_sensor_server sensor_server.c)
target_link_libraries(example_c_sensor_server PRIVATE omnibinder_static)

add_executable(example_c_sensor_client sensor_client.c)
target_link_libraries(example_c_sensor_client PRIVATE omnibinder_static)

# IDL 代码生成（C 语言）
if(TARGET omni-idlc OR TARGET omnic)
    omnic_generate(
        TARGET example_c_sensor_server
        LANGUAGE c
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    omnic_generate(
        TARGET example_c_sensor_client
        LANGUAGE c
        FILES
            ${CMAKE_SOURCE_DIR}/examples/common_types.bidl
            ${CMAKE_SOURCE_DIR}/examples/sensor_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    if(TARGET omni-idlc)
        add_dependencies(example_c_sensor_server omni-idlc)
        add_dependencies(example_c_sensor_client omni-idlc)
    else()
        add_dependencies(example_c_sensor_server omnic)
        add_dependencies(example_c_sensor_client omnic)
    endif()
endif()
