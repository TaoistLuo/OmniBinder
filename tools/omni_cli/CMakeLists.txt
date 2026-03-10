# tools/omni_cli/CMakeLists.txt

add_executable(omni-cli
    main.cpp
    simple_json.cpp
    type_codec.cpp
)

target_link_libraries(omni-cli PRIVATE
    omnibinder_static
    omni_idlc_parser
)

target_include_directories(omni-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/tools/omni_idlc
)

install(TARGETS omni-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
