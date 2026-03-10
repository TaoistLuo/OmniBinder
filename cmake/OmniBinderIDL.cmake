# OmniBinderIDL.cmake - omnic_generate() CMake 函数
#
# 用法:
#   omnic_generate(
#       TARGET <target_name>
#       LANGUAGE <cpp|c|all>
#       FILES <file1.bidl> [file2.bidl ...]
#       OUTPUT_DIR <output_directory>
#   )

function(omnic_generate)
    cmake_parse_arguments(BIDL "" "TARGET;LANGUAGE;OUTPUT_DIR" "FILES" ${ARGN})

    if(NOT BIDL_TARGET)
        message(FATAL_ERROR "omnic_generate: TARGET is required")
    endif()
    if(NOT BIDL_LANGUAGE)
        set(BIDL_LANGUAGE "cpp")
    endif()
    if(NOT BIDL_OUTPUT_DIR)
        set(BIDL_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()
    if(NOT BIDL_FILES)
        message(FATAL_ERROR "omnic_generate: FILES is required")
    endif()

    # 确保输出目录存在
    file(MAKE_DIRECTORY ${BIDL_OUTPUT_DIR})

    if(TARGET omni-idlc)
        set(BINDERC_EXECUTABLE $<TARGET_FILE:omni-idlc>)
    elseif(TARGET omnic)
        set(BINDERC_EXECUTABLE $<TARGET_FILE:omnic>)
    else()
        find_program(BINDERC_EXECUTABLE NAMES omni-idlc omnic)
        if(NOT BINDERC_EXECUTABLE)
            message(FATAL_ERROR "omni-idlc not found")
        endif()
    endif()

    set(GENERATED_SOURCES "")
    set(GENERATED_HEADERS "")

    foreach(BIDL_FILE ${BIDL_FILES})
        get_filename_component(BIDL_NAME ${BIDL_FILE} NAME)
        get_filename_component(BIDL_ABS ${BIDL_FILE} ABSOLUTE)
        get_filename_component(BIDL_DIR ${BIDL_ABS} DIRECTORY)

        if(BIDL_LANGUAGE STREQUAL "cpp" OR BIDL_LANGUAGE STREQUAL "all")
            set(GEN_HEADER "${BIDL_OUTPUT_DIR}/${BIDL_NAME}.h")
            set(GEN_SOURCE "${BIDL_OUTPUT_DIR}/${BIDL_NAME}.cpp")
            set(DEP_FILE "${BIDL_OUTPUT_DIR}/${BIDL_NAME}.cpp.d")
            list(APPEND GENERATED_HEADERS ${GEN_HEADER})
            list(APPEND GENERATED_SOURCES ${GEN_SOURCE})

            add_custom_command(
                OUTPUT ${GEN_HEADER} ${GEN_SOURCE}
                COMMAND ${BINDERC_EXECUTABLE}
                    --lang=cpp
                    --output=${BIDL_OUTPUT_DIR}
                    --dep-file=${DEP_FILE}
                    ${BIDL_ABS}
                DEPENDS ${BIDL_ABS}
                DEPFILE ${DEP_FILE}
                COMMENT "Generating C++ code from ${BIDL_NAME}"
                VERBATIM
            )
        endif()

        if(BIDL_LANGUAGE STREQUAL "c" OR BIDL_LANGUAGE STREQUAL "all")
            set(GEN_C_HEADER "${BIDL_OUTPUT_DIR}/${BIDL_NAME}_c.h")
            set(GEN_C_SOURCE "${BIDL_OUTPUT_DIR}/${BIDL_NAME}.c")
            set(DEP_C_FILE "${BIDL_OUTPUT_DIR}/${BIDL_NAME}.c.d")
            list(APPEND GENERATED_HEADERS ${GEN_C_HEADER})
            list(APPEND GENERATED_SOURCES ${GEN_C_SOURCE})

            add_custom_command(
                OUTPUT ${GEN_C_HEADER} ${GEN_C_SOURCE}
                COMMAND ${BINDERC_EXECUTABLE}
                    --lang=c
                    --output=${BIDL_OUTPUT_DIR}
                    --dep-file=${DEP_C_FILE}
                    ${BIDL_ABS}
                DEPENDS ${BIDL_ABS}
                DEPFILE ${DEP_C_FILE}
                COMMENT "Generating C code from ${BIDL_NAME}"
                VERBATIM
            )
        endif()
    endforeach()

    # 将生成的源文件添加到目标
    target_sources(${BIDL_TARGET} PRIVATE ${GENERATED_SOURCES} ${GENERATED_HEADERS})
    target_include_directories(${BIDL_TARGET} PRIVATE ${BIDL_OUTPUT_DIR})
endfunction()
