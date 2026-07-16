# OmniBinder - 应用层微服务通信框架
# 顶层 CMakeLists.txt

cmake_minimum_required(VERSION 3.10)
project(OmniBinder VERSION 1.0.0 LANGUAGES C CXX)

# ============================================================
# 编译选项
# ============================================================
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 默认构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# 默认安装目录：构建目录下的 install/
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install path" FORCE)
endif()

# 编译选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O2)
    endif()
elseif(MSVC)
    add_compile_options(/W4)
endif()

# ============================================================
# 平台检测
# ============================================================
include(cmake/Platform.cmake)
include(cmake/OmniBinderIDL.cmake)

# ============================================================
# 安装目录
# ============================================================
include(GNUInstallDirs)

# ============================================================
# 构建输出目录
# ============================================================
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/lib)

# ============================================================
# 选项
# ============================================================
option(OMNIBINDER_BUILD_TESTS "Build unit tests" ON)
option(OMNIBINDER_BUILD_EXAMPLES "Build examples" ON)
option(OMNIBINDER_BUILD_TOOLS "Build tools (omni-idlc, omni-cli)" ON)
set(OMNIBINDER_HOST_IDLC "" CACHE FILEPATH "Path to a host-runnable omni-idlc executable")

get_filename_component(_omnibinder_c_compiler_name "${CMAKE_C_COMPILER}" NAME)
set(_omnibinder_is_cross_env OFF)
if(CMAKE_CROSSCOMPILING)
    set(_omnibinder_is_cross_env ON)
elseif(_omnibinder_c_compiler_name MATCHES "^(arm|aarch64|riscv|mips)[^/]*(gcc|g\\+\\+|clang)")
    set(_omnibinder_is_cross_env ON)
endif()

set(_omnibinder_build_host_idlc_default ON)
if(_omnibinder_is_cross_env)
    set(_omnibinder_build_host_idlc_default OFF)
endif()
option(OMNIBINDER_BUILD_HOST_IDLC "Build the omni-idlc executable" ${_omnibinder_build_host_idlc_default})

set(OMNIBINDER_INSTALL_HOST_BINDIR "bin_host")
set(OMNIBINDER_INSTALL_CROSS_BINDIR "bin_cross")
if(_omnibinder_is_cross_env)
    set(OMNIBINDER_INSTALL_RUNTIME_BINDIR ${OMNIBINDER_INSTALL_CROSS_BINDIR})
    set(OMNIBINDER_BUILD_CONTEXT "cross")
else()
    set(OMNIBINDER_INSTALL_RUNTIME_BINDIR ${OMNIBINDER_INSTALL_HOST_BINDIR})
    set(OMNIBINDER_BUILD_CONTEXT "host")
endif()

# ============================================================
# 公共头文件目录
# ============================================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# ============================================================
# 子目录
# ============================================================

# 核心库
add_subdirectory(src)

# ServiceManager
add_subdirectory(service_manager)

# 工具
if(OMNIBINDER_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# 示例
if(OMNIBINDER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 测试
if(OMNIBINDER_BUILD_TESTS)
    enable_testing()

    # 抑制 googletest 对 C++17 特性的检测警告（项目使用 C++11）
    set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard" FORCE)

    # 禁用 googletest 自带的所有选项
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_disable_pthreads OFF CACHE BOOL "" FORCE)

    add_subdirectory(thirdparty/googletest-release-1.10.0)

    # 恢复项目 C++ 标准
    set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard" FORCE)

    add_subdirectory(tests)
endif()

# ============================================================
# 安装配置
# ============================================================
# 安装头文件
install(DIRECTORY include/omnibinder
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# 安装 CMake 配置文件
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OmniBinderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/OmniBinderConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniBinder
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/OmniBinderConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/OmniBinderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/OmniBinderConfigVersion.cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/OmniBinderIDL.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OmniBinder
)

# ============================================================
# 打印配置信息
# ============================================================
message(STATUS "")
message(STATUS "OmniBinder Configuration:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform:       ${OMNIBINDER_PLATFORM}")
message(STATUS "  Build context:  ${OMNIBINDER_BUILD_CONTEXT}")
message(STATUS "  C++ Compiler:   ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build tests:    ${OMNIBINDER_BUILD_TESTS}")
message(STATUS "  Build examples: ${OMNIBINDER_BUILD_EXAMPLES}")
message(STATUS "  Build tools:    ${OMNIBINDER_BUILD_TOOLS}")
message(STATUS "  Build host idlc:${OMNIBINDER_BUILD_HOST_IDLC}")
message(STATUS "")
