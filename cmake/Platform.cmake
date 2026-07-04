# Platform.cmake - 平台检测和配置

# 检测操作系统
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OMNIBINDER_PLATFORM "Linux")
    set(OMNIBINDER_LINUX TRUE)
    add_definitions(-DOMNIBINDER_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(OMNIBINDER_PLATFORM "Windows")
    set(OMNIBINDER_WINDOWS TRUE)
    add_definitions(-DOMNIBINDER_WINDOWS)
    add_definitions(-D_WIN32_WINNT=0x0601)  # Windows 7+
    add_definitions(-DNOMINMAX)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}. OmniBinder supports Linux and Windows only.")
endif()

# 查找平台依赖库
if(OMNIBINDER_LINUX)
    find_package(Threads REQUIRED)
    set(OMNIBINDER_PLATFORM_LIBS Threads::Threads)
    include(CheckLibraryExists)
    check_library_exists(rt shm_open "" HAVE_LIBRT)
    if(HAVE_LIBRT)
        list(APPEND OMNIBINDER_PLATFORM_LIBS rt)
    endif()
elseif(OMNIBINDER_WINDOWS)
    set(OMNIBINDER_PLATFORM_LIBS
        ws2_32      # Winsock
    )
endif()
