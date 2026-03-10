# tests/CMakeLists.txt

# 测试可执行文件输出到 target/test
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/test)

# 简单测试框架（不引入外部依赖）
add_executable(test_buffer test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE omnibinder_static)
add_test(NAME test_buffer COMMAND test_buffer)

add_executable(test_message test_message.cpp)
target_link_libraries(test_message PRIVATE omnibinder_static)
add_test(NAME test_message COMMAND test_message)

add_executable(test_event_loop test_event_loop.cpp)
target_link_libraries(test_event_loop PRIVATE omnibinder_static)
add_test(NAME test_event_loop COMMAND test_event_loop)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE omnibinder_static)
add_test(NAME test_transport COMMAND test_transport)

add_executable(test_service_registry
    test_service_registry.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/service_registry.cpp
)
target_include_directories(test_service_registry PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_service_registry PRIVATE omnibinder_static)
add_test(NAME test_service_registry COMMAND test_service_registry)

add_executable(test_heartbeat_death
    test_heartbeat_death.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/heartbeat_monitor.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/death_notifier.cpp
)
target_include_directories(test_heartbeat_death PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_heartbeat_death PRIVATE omnibinder_static)
add_test(NAME test_heartbeat_death COMMAND test_heartbeat_death)

add_executable(test_topic_manager
    test_topic_manager.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/topic_manager.cpp
)
target_include_directories(test_topic_manager PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_topic_manager PRIVATE omnibinder_static)
add_test(NAME test_topic_manager COMMAND test_topic_manager)

add_executable(test_shm_transport test_shm_transport.cpp)
target_link_libraries(test_shm_transport PRIVATE omnibinder_static)
add_test(NAME test_shm_transport COMMAND test_shm_transport)

add_executable(test_idl_compiler
    test_idl_compiler.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/parser.cpp
)
target_include_directories(test_idl_compiler PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
    ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME test_idl_compiler COMMAND test_idl_compiler)

add_executable(test_omni_cli_type_codec
    test_omni_tool_type_codec.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_cli/simple_json.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_cli/type_codec.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/parser.cpp
)
target_include_directories(test_omni_cli_type_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/omni_cli
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_omni_cli_type_codec PRIVATE omnibinder_static)
add_test(NAME test_omni_cli_type_codec COMMAND test_omni_cli_type_codec)

add_executable(test_c_api test_c_api.cpp)
target_link_libraries(test_c_api PRIVATE omnibinder_static pthread)
add_dependencies(test_c_api service_manager)
add_test(NAME test_c_api COMMAND test_c_api
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_c_api PROPERTIES TIMEOUT 120)

add_executable(test_soak_runner test_soak_runner.cpp)
add_dependencies(test_soak_runner
    test_full_integration
    test_control_plane_and_fallback
    test_threadsafe_client_and_reconnect
    test_performance
)
add_test(NAME test_soak_runner COMMAND test_soak_runner 2
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_soak_runner PROPERTIES TIMEOUT 300)

add_executable(test_transient_disconnect_recovery test_transient_disconnect_recovery.cpp)
target_link_libraries(test_transient_disconnect_recovery PRIVATE omnibinder_static pthread)
add_dependencies(test_transient_disconnect_recovery service_manager)
add_test(NAME test_transient_disconnect_recovery COMMAND test_transient_disconnect_recovery
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_transient_disconnect_recovery PROPERTIES TIMEOUT 180)

# 端到端集成测试（需要 service_manager 可执行文件）
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE omnibinder_static)
add_dependencies(test_integration service_manager)
add_test(NAME test_integration COMMAND test_integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_integration PROPERTIES TIMEOUT 60)

# 完整集成测试（SHM多客户端、广播订阅、死亡通知）
add_executable(test_full_integration test_full_integration.cpp)
target_link_libraries(test_full_integration PRIVATE omnibinder_static)
target_link_libraries(test_full_integration PRIVATE pthread)
add_dependencies(test_full_integration service_manager)
add_test(NAME test_full_integration COMMAND test_full_integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_full_integration PROPERTIES TIMEOUT 90)

add_executable(test_control_plane_and_fallback test_control_plane_and_fallback.cpp)
target_link_libraries(test_control_plane_and_fallback PRIVATE omnibinder_static)
target_link_libraries(test_control_plane_and_fallback PRIVATE pthread)
add_dependencies(test_control_plane_and_fallback service_manager)
add_test(NAME test_control_plane_and_fallback COMMAND test_control_plane_and_fallback
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_control_plane_and_fallback PROPERTIES TIMEOUT 90)

# 性能基准测试（RPC 往返延迟、话题发布/订阅延迟）
add_executable(test_performance test_performance.cpp)
target_link_libraries(test_performance PRIVATE omnibinder_static)
target_link_libraries(test_performance PRIVATE pthread)
add_dependencies(test_performance service_manager)
add_test(NAME test_performance COMMAND test_performance
    --report ${CMAKE_SOURCE_DIR}/docs/performance-report.md
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_performance PROPERTIES TIMEOUT 120)

add_executable(test_threadsafe_client_and_reconnect test_threadsafe_client_and_reconnect.cpp)
target_link_libraries(test_threadsafe_client_and_reconnect PRIVATE omnibinder_static)
target_link_libraries(test_threadsafe_client_and_reconnect PRIVATE pthread)
add_dependencies(test_threadsafe_client_and_reconnect service_manager)
add_test(NAME test_threadsafe_client_and_reconnect COMMAND test_threadsafe_client_and_reconnect
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_threadsafe_client_and_reconnect PROPERTIES TIMEOUT 120)

add_executable(test_runtime_stats test_runtime_stats.cpp)
target_link_libraries(test_runtime_stats PRIVATE omnibinder_static)
target_link_libraries(test_runtime_stats PRIVATE pthread)
add_dependencies(test_runtime_stats service_manager)
add_test(NAME test_runtime_stats COMMAND test_runtime_stats
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_runtime_stats PROPERTIES TIMEOUT 120)

add_executable(test_error_logging test_error_logging.cpp)
target_link_libraries(test_error_logging PRIVATE omnibinder_static)
target_link_libraries(test_error_logging PRIVATE pthread)
add_dependencies(test_error_logging service_manager)
add_test(NAME test_error_logging COMMAND test_error_logging
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_error_logging PROPERTIES TIMEOUT 120)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_longrun_tools
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_longrun_tools.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_longrun_tools PROPERTIES TIMEOUT 30)
endif()
