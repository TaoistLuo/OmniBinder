# service_manager/CMakeLists.txt

add_executable(service_manager
    main.cpp
    service_registry.cpp
    heartbeat_monitor.cpp
    death_notifier.cpp
    topic_manager.cpp
)

target_link_libraries(service_manager PRIVATE omnibinder_static)

install(TARGETS service_manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
