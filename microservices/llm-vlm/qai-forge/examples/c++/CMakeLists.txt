# ─────────────────────────────────────────────────────────────────────────────
# qai-forge C++ Examples
#
# Build from the repo root:
#   cmake -S qai-forge/examples/c++ -B qai-forge/examples/c++/build \
#         -DCMAKE_PREFIX_PATH=<qai-forge install prefix>
#   cmake --build qai-forge/examples/c++/build --parallel
#
# Each example is a standalone executable that links qai_forge.
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)
project(QaiForgeExamples CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nlohmann_json REQUIRED)
find_package(qai_forge REQUIRED)

# Shared include path for all examples
set(SDK_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

function(add_example name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${SDK_INCLUDE})
    target_link_libraries(${name} PRIVATE
        qai_forge::qai_forge
        nlohmann_json::nlohmann_json
    )
endfunction()

add_example(01_simple_chat       01_simple_chat/main.cpp)
add_example(02_streaming_chat    02_streaming_chat/main.cpp)
add_example(03_multi_turn        03_multi_turn/main.cpp)
add_example(04_reasoning         04_reasoning/main.cpp)
add_example(05_vlm               05_vlm/main.cpp)
add_example(06_tool_calling      06_tool_calling/main.cpp)

install(TARGETS
    01_simple_chat
    02_streaming_chat
    03_multi_turn
    04_reasoning
    05_vlm
    06_tool_calling
    DESTINATION bin/examples
)
