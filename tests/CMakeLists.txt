# tests/CMakeLists.txt
#
# 测试可执行文件输出到 target/test
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/test)

# 公共测试头文件路径
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ============================================================
# 单元测试（使用 GTest）
# ============================================================

add_executable(test_buffer test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_buffer COMMAND test_buffer)

add_executable(test_message test_message.cpp)
target_link_libraries(test_message PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_message COMMAND test_message)

add_executable(test_event_loop test_event_loop.cpp)
target_link_libraries(test_event_loop PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_event_loop COMMAND test_event_loop)

add_executable(test_owner_thread_executor test_owner_thread_executor.cpp)
target_include_directories(test_owner_thread_executor PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_owner_thread_executor PRIVATE omnibinder_static gtest gtest_main)
if(CMAKE_USE_PTHREADS_INIT)
    target_link_libraries(test_owner_thread_executor PRIVATE Threads::Threads)
endif()
add_test(NAME test_owner_thread_executor COMMAND test_owner_thread_executor)

add_executable(test_transport test_transport.cpp)
target_link_libraries(test_transport PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_transport COMMAND test_transport)

add_executable(test_service_registry
    test_service_registry.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/service_registry.cpp
)
target_include_directories(test_service_registry PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_service_registry PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_service_registry COMMAND test_service_registry)

add_executable(test_heartbeat_death
    test_heartbeat_death.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/heartbeat_monitor.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/death_notifier.cpp
)
target_include_directories(test_heartbeat_death PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_heartbeat_death PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_heartbeat_death COMMAND test_heartbeat_death)

add_executable(test_topic_manager
    test_topic_manager.cpp
    ${CMAKE_SOURCE_DIR}/service_manager/topic_manager.cpp
)
target_include_directories(test_topic_manager PRIVATE ${CMAKE_SOURCE_DIR}/service_manager)
target_link_libraries(test_topic_manager PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_topic_manager COMMAND test_topic_manager)

add_executable(test_shm_transport test_shm_transport.cpp)
target_link_libraries(test_shm_transport PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_shm_transport COMMAND test_shm_transport)

add_executable(test_idl_compiler
    test_idl_compiler.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/codegen_cpp.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/codegen_c.cpp
)
target_include_directories(test_idl_compiler PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_idl_compiler PRIVATE gtest gtest_main)
add_test(NAME test_idl_compiler COMMAND test_idl_compiler)

add_executable(test_omni_cli_type_codec
    test_omni_tool_type_codec.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_cli/simple_json.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_cli/type_resolver.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_cli/type_codec.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/parser.cpp
)
target_include_directories(test_omni_cli_type_codec PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/omni_cli
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_omni_cli_type_codec PRIVATE omnibinder_static gtest gtest_main)
add_test(NAME test_omni_cli_type_codec COMMAND test_omni_cli_type_codec)

# ============================================================
# 集成测试 — pthread 已替换为 std::thread，Windows 可编译
# ============================================================

add_executable(test_stats test_stats.cpp)
target_link_libraries(test_stats PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_stats service_manager)
add_test(NAME test_stats COMMAND test_stats WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_stats PROPERTIES TIMEOUT 120)

add_executable(test_c_api test_c_api.cpp)
target_link_libraries(test_c_api PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_c_api service_manager)
add_test(NAME test_c_api COMMAND test_c_api WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_c_api PROPERTIES TIMEOUT 120)

add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_integration service_manager)
add_test(NAME test_integration COMMAND test_integration WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_integration PROPERTIES TIMEOUT 60)

add_executable(test_threadsafe_client_and_reconnect test_threadsafe_client_and_reconnect.cpp)
target_link_libraries(test_threadsafe_client_and_reconnect PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_threadsafe_client_and_reconnect service_manager)
add_test(NAME test_threadsafe_client_and_reconnect COMMAND test_threadsafe_client_and_reconnect WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_threadsafe_client_and_reconnect PROPERTIES TIMEOUT 120)

add_executable(test_runtime_stats test_runtime_stats.cpp)
target_link_libraries(test_runtime_stats PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_runtime_stats service_manager)
add_test(NAME test_runtime_stats COMMAND test_runtime_stats WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_runtime_stats PROPERTIES TIMEOUT 120)

add_executable(test_error_logging test_error_logging.cpp)
target_link_libraries(test_error_logging PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_error_logging service_manager)
add_test(NAME test_error_logging COMMAND test_error_logging WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_error_logging PROPERTIES TIMEOUT 120)

add_executable(test_thread_safety test_thread_safety.cpp)
target_link_libraries(test_thread_safety PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_thread_safety service_manager)
add_test(NAME test_thread_safety COMMAND test_thread_safety WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_thread_safety PROPERTIES TIMEOUT 600)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_sm_reconnect.cpp)
    add_executable(test_sm_reconnect test_sm_reconnect.cpp)
    target_link_libraries(test_sm_reconnect PRIVATE omnibinder_static gtest gtest_main)
    add_dependencies(test_sm_reconnect service_manager)
    add_test(NAME test_sm_reconnect COMMAND test_sm_reconnect WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(test_sm_reconnect PROPERTIES TIMEOUT 120)
endif()

add_executable(test_control_plane_and_fallback test_control_plane_and_fallback.cpp)
target_link_libraries(test_control_plane_and_fallback PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_control_plane_and_fallback service_manager)
add_test(NAME test_control_plane_and_fallback COMMAND test_control_plane_and_fallback WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_control_plane_and_fallback PROPERTIES TIMEOUT 600)

add_executable(test_full_integration test_full_integration.cpp)
target_link_libraries(test_full_integration PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_full_integration service_manager)
add_test(NAME test_full_integration COMMAND test_full_integration WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_full_integration PROPERTIES TIMEOUT 90)

add_executable(test_heartbeat_reconnect test_heartbeat_reconnect.cpp)
target_link_libraries(test_heartbeat_reconnect PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_heartbeat_reconnect service_manager)
add_test(NAME test_heartbeat_reconnect COMMAND test_heartbeat_reconnect WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_heartbeat_reconnect PROPERTIES TIMEOUT 120)

# ============================================================
# 集成测试 — 依赖 POSIX fork() 的测试，仅 Linux 编译
# ============================================================
if(NOT WIN32)

add_executable(test_idl_mismatch test_idl_mismatch.cpp)
target_link_libraries(test_idl_mismatch PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_idl_mismatch service_manager)
add_test(NAME test_idl_mismatch COMMAND test_idl_mismatch WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_idl_mismatch PROPERTIES TIMEOUT 60)

add_executable(test_generated_runtime_integration
    test_generated_runtime_integration.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/lexer.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/codegen_cpp.cpp
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc/codegen_c.cpp
)
target_include_directories(test_generated_runtime_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(test_generated_runtime_integration PRIVATE
    OMNI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    OMNI_BUILD_DIR="${CMAKE_BINARY_DIR}"
)
target_link_libraries(test_generated_runtime_integration PRIVATE omnibinder_static gtest gtest_main)
add_dependencies(test_generated_runtime_integration service_manager)
add_test(NAME test_generated_runtime_integration COMMAND test_generated_runtime_integration WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_generated_runtime_integration PROPERTIES TIMEOUT 300)

endif()

# ============================================================
# 性能测试与稳定性测试
# ============================================================

add_executable(test_performance test_performance.cpp)
target_link_libraries(test_performance PRIVATE omnibinder_static)
target_include_directories(test_performance PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/perf_idl)
add_dependencies(test_performance service_manager)

add_executable(test_performance_server
    test_performance_server.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/perf_idl/perf_service.bidl.cpp
)
target_link_libraries(test_performance_server PRIVATE omnibinder_static)
target_include_directories(test_performance_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/perf_idl)
add_dependencies(test_performance_server service_manager test_performance)

if(TARGET omni-idlc)
    omnic_generate(TARGET test_performance LANGUAGE cpp
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/perf_service.bidl
        OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/perf_idl)
else()
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/perf_idl/perf_service.bidl.h
        COMMAND ${CMAKE_BINARY_DIR}/target/bin/omni-idlc
            ${CMAKE_CURRENT_SOURCE_DIR}/perf_service.bidl
            --lang cpp --outdir ${CMAKE_CURRENT_BINARY_DIR}/perf_idl
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/perf_service.bidl omni-idlc)
endif()
add_test(NAME test_performance COMMAND test_performance
    -o ${CMAKE_SOURCE_DIR}/docs/performance-report.md
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_tests_properties(test_performance PROPERTIES TIMEOUT 120)

add_executable(test_soak_runner test_soak_runner.cpp)
add_dependencies(test_soak_runner
    test_performance
    test_full_integration
    test_control_plane_and_fallback
    test_threadsafe_client_and_reconnect
)
add_test(NAME test_soak_runner COMMAND test_soak_runner 2 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_soak_runner PROPERTIES TIMEOUT 900)

add_executable(test_transient_disconnect_recovery test_transient_disconnect_recovery.cpp)
target_link_libraries(test_transient_disconnect_recovery PRIVATE omnibinder_static)
add_dependencies(test_transient_disconnect_recovery service_manager)
add_test(NAME test_transient_disconnect_recovery COMMAND test_transient_disconnect_recovery WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_transient_disconnect_recovery PROPERTIES TIMEOUT 180)

set_tests_properties(
    test_performance
    test_transient_disconnect_recovery
    test_soak_runner
    PROPERTIES RESOURCE_LOCK sm_port_lock
)
if(NOT WIN32)
set_tests_properties(
    test_full_integration
    test_control_plane_and_fallback
    test_error_logging
    test_threadsafe_client_and_reconnect
    PROPERTIES RESOURCE_LOCK sm_port_lock
)
endif()

# ============================================================
# Python 测试
# ============================================================

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_longrun_tools
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_longrun_tools.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(test_longrun_tools PROPERTIES TIMEOUT 30)
endif()
