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
    message(WARNING "Unsupported platform: ${CMAKE_SYSTEM_NAME}, trying POSIX fallback")
    set(OMNIBINDER_PLATFORM "POSIX")
    set(OMNIBINDER_LINUX TRUE)
    add_definitions(-DOMNIBINDER_LINUX)
endif()

# 查找平台依赖库
if(OMNIBINDER_LINUX)
    find_package(Threads REQUIRED)
    set(OMNIBINDER_PLATFORM_LIBS
        Threads::Threads
        rt          # POSIX shared memory, semaphores
    )
elseif(OMNIBINDER_WINDOWS)
    set(OMNIBINDER_PLATFORM_LIBS
        ws2_32      # Winsock
    )
endif()
