# examples/CMakeLists.txt

include(${CMAKE_SOURCE_DIR}/cmake/OmniBinderIDL.cmake)

# 示例可执行文件输出到 target/example
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/target/example)

add_subdirectory(example_cpp)
add_subdirectory(example_c)
