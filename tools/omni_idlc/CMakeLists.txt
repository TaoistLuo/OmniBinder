# tools/omni_idlc/CMakeLists.txt

add_library(omni_idlc_parser STATIC
    lexer.cpp
    parser.cpp
)

target_include_directories(omni_idlc_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

add_executable(omni-idlc
    main.cpp
    codegen_cpp.cpp
    codegen_c.cpp
)

target_link_libraries(omni-idlc PRIVATE omni_idlc_parser)
target_include_directories(omni-idlc PRIVATE ${CMAKE_SOURCE_DIR}/include)

install(TARGETS omni-idlc
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
