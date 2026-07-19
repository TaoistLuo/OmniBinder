# src/CMakeLists.txt - OmniBinder 核心库

set(OMNIBINDER_SOURCES
    # core
    core/log.cpp
    core/buffer.cpp
    core/message.cpp
    core/error.cpp
    core/event_loop.cpp
    core/omni_allocator.cpp
    core/service.cpp
    core/omni_runtime.cpp
    core/runtime_helpers.cpp
    core/runtime_sm.cpp
    core/runtime_service.cpp
    core/runtime_connection.cpp
    core/runtime_rpc.cpp
    core/runtime_topic.cpp
    core/runtime_dispatch.cpp
    core/runtime_diag.cpp
    core/sm_control_channel.cpp
    core/rpc_runtime.cpp
    core/topic_runtime.cpp
    core/connection_manager.cpp
    core/omnibinder_c.cpp

    # transport
    transport/tcp_transport.cpp
    transport/shm_transport.cpp
    transport/transport_selector.cpp

    # platform
    platform/platform_linux.cpp
    platform/platform_win.cpp
    platform/event_backend_linux.cpp
    platform/event_backend_win.cpp
)

# 创建静态库和共享库
add_library(omnibinder_static STATIC ${OMNIBINDER_SOURCES})
target_include_directories(omnibinder_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(omnibinder_static PUBLIC ${OMNIBINDER_PLATFORM_LIBS})
set_target_properties(omnibinder_static PROPERTIES OUTPUT_NAME omnibinder)

add_library(omnibinder SHARED ${OMNIBINDER_SOURCES})
target_include_directories(omnibinder PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(omnibinder PUBLIC ${OMNIBINDER_PLATFORM_LIBS})
set_target_properties(omnibinder PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# 安装
install(TARGETS omnibinder omnibinder_static
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
