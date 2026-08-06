cmake_minimum_required(VERSION 3.16)
project(QAIServe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(qai_forge REQUIRED)
find_package(CURL REQUIRED)
find_package(libzip REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Source files
# ─────────────────────────────────────────────────────────────────────────────
set(SERVER_SOURCES
    # HTTP layer
    src/main.cpp
    src/controllers/ResponsesController.cpp
    src/controllers/InferController.cpp

    # Admin model management (admin-only, requires X-Admin-Token)
    src/controllers/AdminController.cpp
    src/admin/AiHubClient.cpp
    src/admin/ModelFetchJob.cpp

    # OIP v2 — model discovery (GET /v1/models, GET /v2/models, GET /v2/models/{model})
    src/controllers/ModelsController.cpp

    # OIP v2 — inference (POST /v2/models/{model}/infer|generate|generate_stream)
    # InferController.cpp already listed above; add OIP parsing helpers
    src/oip/OipDTOs.cpp
    src/oip/OipBinaryParser.cpp
    src/oip/MultipartParser.cpp

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

add_executable(qaiserve ${SERVER_SOURCES})

# ─────────────────────────────────────────────────────────────────────────────
# Include directories
# ─────────────────────────────────────────────────────────────────────────────
target_include_directories(qaiserve PRIVATE
    # QAIServe own headers (controllers/, mcp/, ws/, tools/, ResponsesUtils.h, admin/)
    ${CMAKE_CURRENT_SOURCE_DIR}/include

    # qai-forge SDK headers (sibling directory)
    ${CMAKE_CURRENT_SOURCE_DIR}/../qai-forge/include
)

# ─────────────────────────────────────────────────────────────────────────────
# Link libraries
# ─────────────────────────────────────────────────────────────────────────────
target_link_libraries(qaiserve PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    qai_forge::qai_forge
    CURL::libcurl
    libzip::zip
)

# ─────────────────────────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS qaiserve DESTINATION bin)

# Install the default MCP server config (if it exists)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/mcp_servers.json
            DESTINATION bin)
endif()
