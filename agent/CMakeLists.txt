cmake_minimum_required(VERSION 3.21)
project(prosperotrace C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build with:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=$PS5_PAYLOAD_SDK/cmake/toolchain-ps5.cmake
#   cmake --build build

# generated NID table (tools/nidgen.py data/nids.txt)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/gen/nids_table.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/../tools/nidgen.py
            ${CMAKE_SOURCE_DIR}/data/nids.txt > ${CMAKE_BINARY_DIR}/gen/nids_table.c
    DEPENDS ${CMAKE_SOURCE_DIR}/data/nids.txt ${CMAKE_SOURCE_DIR}/../tools/nidgen.py
    COMMENT "generating nids_table.c"
)

add_executable(prosperotrace
    src/main.c
    src/stream.c
    src/target.c
    src/hook.c
    src/trampoline.c
    src/ring_decode.c
    src/filter.c
    src/nids.c
    ${CMAKE_BINARY_DIR}/gen/nids_table.c
)

target_include_directories(prosperotrace PRIVATE src)
target_compile_options(prosperotrace PRIVATE -Wall -Wextra -O2)
set_target_properties(prosperotrace PROPERTIES OUTPUT_NAME prosperotrace SUFFIX .elf)
