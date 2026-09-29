# src/CMakeLists.txt - OmniBinder 核心库

set(OMNIBINDER_SOURCES
    # core
    core/log.cpp
    core/buffer.cpp
    core/buffer_view.cpp
    core/message.cpp
    core/message_reader.cpp
    core/error.cpp
    core/event_loop.cpp
    core/omni_allocator.cpp
    core/service.cpp
    core/proxy_base.cpp
    core/omni_runtime.cpp
    core/runtime_helpers.cpp
    core/runtime_sm.cpp
    core/runtime_service.cpp
    core/runtime_connection.cpp
    core/runtime_rpc.cpp
    core/runtime_topic.cpp
    core/runtime_dispatch.cpp
    core/runtime_diag.cpp
    core/runtime_config.cpp
    core/sm_control_channel.cpp
    core/pending_reply_table.cpp
    core/rpc_runtime.cpp
    core/topic_runtime.cpp
    core/connection_manager.cpp
    core/omnibinder_c.cpp

    # transport
    transport/tcp_connection.cpp
    transport/tcp_server_endpoint.cpp
    transport/shm_ring.cpp
    transport/shm_client_connection.cpp
    transport/shm_server_endpoint.cpp
    transport/shm_server_connection.cpp
    transport/transport_selector.cpp

    # platform
    platform/platform_common.cpp
    platform/platform_linux.cpp
    platform/platform_win.cpp
    platform/event_backend_linux.cpp
    platform/event_backend_win.cpp
)

# 一次编译：对象库（PIC），静态库归档与动态库链接各自复用同一批 .o
add_library(omnibinder_obj OBJECT ${OMNIBINDER_SOURCES})
set_target_properties(omnibinder_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(omnibinder_obj PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# 创建静态库（复用对象库，不重复编译）
add_library(omnibinder_static STATIC $<TARGET_OBJECTS:omnibinder_obj>)
target_include_directories(omnibinder_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(omnibinder_static PUBLIC ${OMNIBINDER_PLATFORM_LIBS})
target_compile_definitions(omnibinder_static PUBLIC ${OMNIBINDER_VERSION_DEFS})
set_target_properties(omnibinder_static PROPERTIES OUTPUT_NAME omnibinder)

# 创建动态库（-DOMNIBINDER_BUILD_SHARED=OFF 可关闭；复用同一对象库）
if(OMNIBINDER_BUILD_SHARED)
    add_library(omnibinder_shared SHARED $<TARGET_OBJECTS:omnibinder_obj>)
    target_include_directories(omnibinder_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(omnibinder_shared PUBLIC ${OMNIBINDER_PLATFORM_LIBS})
    target_compile_definitions(omnibinder_shared PUBLIC ${OMNIBINDER_VERSION_DEFS})
    set_target_properties(omnibinder_shared PROPERTIES
        OUTPUT_NAME omnibinder
        POSITION_INDEPENDENT_CODE ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
    if(MSVC)
        # MSVC 下静态库与动态库导入库同名（omnibinder.lib），错开避免互相覆盖
        set_target_properties(omnibinder_shared PROPERTIES ARCHIVE_OUTPUT_NAME omnibinder_shared)
    endif()
endif()

# 安装（静态库 + 动态库）
set(OMNIBINDER_LIB_TARGETS omnibinder_static)
if(OMNIBINDER_BUILD_SHARED)
    list(APPEND OMNIBINDER_LIB_TARGETS omnibinder_shared)
endif()

install(TARGETS ${OMNIBINDER_LIB_TARGETS}
    EXPORT OmniBinderTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT OmniBinderTargets
    FILE OmniBinderTargets.cmake
    NAMESPACE OmniBinder::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniBinder
)
