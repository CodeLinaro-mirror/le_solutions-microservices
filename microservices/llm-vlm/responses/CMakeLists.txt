cmake_minimum_required(VERSION 3.16)
project(ResponsesServer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(qai_forge REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Source files
# ─────────────────────────────────────────────────────────────────────────────
set(SERVER_SOURCES
    # HTTP layer
    src/main.cpp
    src/controllers/ResponsesController.cpp

    # Shared utilities (used by HTTP + WebSocket controllers)
    src/ResponsesUtils.cpp

    # MCP Layer 1.5 — client infrastructure
    src/mcp/McpStdioClient.cpp
    src/mcp/McpHttpClient.cpp
    src/mcp/McpClientRegistry.cpp
    src/mcp/McpAgenticLoop.cpp

    # Native Tool Registry — in-process tool execution (no subprocess/network)
    # Transport type "native" in McpClientRegistry routes here.
    src/mcp/NativeToolRegistry.cpp
    src/mcp/McpNativeClient.cpp

    # Built-in native tools — add new tools here + one line in main.cpp
    src/tools/DateTimeTool.cpp
    src/tools/CalculatorTool.cpp

    # WebSocket layer — same Drogon instance, same port as HTTP
    src/ws/WsProtocol.cpp
    src/ws/WsConnectionState.cpp
    src/ws/WsResponsesController.cpp
)

add_executable(responses-server ${SERVER_SOURCES})

# ─────────────────────────────────────────────────────────────────────────────
# Include directories
# ─────────────────────────────────────────────────────────────────────────────
target_include_directories(responses-server PRIVATE
    # responses/ own headers (controllers/, mcp/, ws/, tools/, ResponsesUtils.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/include

    # qai-forge SDK headers (sibling directory)
    ${CMAKE_CURRENT_SOURCE_DIR}/../qai-forge/include
)

# ─────────────────────────────────────────────────────────────────────────────
# Link libraries
# ─────────────────────────────────────────────────────────────────────────────
target_link_libraries(responses-server PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    qai_forge::qai_forge
)

# ─────────────────────────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS responses-server DESTINATION bin)

# Install the default MCP server config (if it exists)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json
            DESTINATION bin)
endif()
