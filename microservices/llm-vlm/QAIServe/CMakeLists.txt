cmake_minimum_required(VERSION 3.16)
project(QAIServe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Drogon REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(qai_forge REQUIRED)
find_package(CURL REQUIRED)
find_package(libzip REQUIRED)
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# gRPC's own CMake package (gRPCConfig.cmake) unconditionally expects an
# imported target named c-ares::cares in its link interface. Debian/Ubuntu's
# libc-ares-dev package ships only a pkg-config file (libcares.pc), not a
# CMake config (no c-ares-config.cmake), so plain find_package(gRPC REQUIRED)
# fails with "c-ares::cares ... target was not found". Synthesize that target
# from pkg-config before gRPC's config is processed.
# ─────────────────────────────────────────────────────────────────────────────
if(NOT TARGET c-ares::cares)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CARES REQUIRED IMPORTED_TARGET libcares)
    add_library(c-ares::cares ALIAS PkgConfig::CARES)
endif()

find_package(gRPC REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Proto codegen — qai_forge.proto lives in the sibling qai-forge/ SDK tree.
#
# protobuf_generate() only emits message code (qai_forge.pb.{h,cc}); service
# stubs (qai_forge.grpc.pb.{h,cc}) require an explicit invocation of protoc
# with the grpc_cpp_plugin, which CMake's bundled protobuf module does not
# wire up automatically.
# ─────────────────────────────────────────────────────────────────────────────
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../qai-forge/api/grpc/qai_forge.proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_GEN_HDR ${GENERATED_DIR}/qai_forge.pb.h)
set(PROTO_GEN_SRC ${GENERATED_DIR}/qai_forge.pb.cc)
set(GRPC_GEN_HDR ${GENERATED_DIR}/qai_forge.grpc.pb.h)
set(GRPC_GEN_SRC ${GENERATED_DIR}/qai_forge.grpc.pb.cc)

add_custom_command(
    OUTPUT ${PROTO_GEN_SRC} ${PROTO_GEN_HDR} ${GRPC_GEN_SRC} ${GRPC_GEN_HDR}
    COMMAND $<TARGET_FILE:protobuf::protoc>
        --cpp_out=${GENERATED_DIR}
        --grpc_out=${GENERATED_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        -I ${CMAKE_CURRENT_SOURCE_DIR}/../qai-forge/api/grpc
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf/gRPC C++ stubs from qai_forge.proto"
    VERBATIM)

add_custom_target(qai_forge_proto_gen DEPENDS ${PROTO_GEN_SRC} ${GRPC_GEN_SRC})

# ─────────────────────────────────────────────────────────────────────────────
# GenieX SDK — native model-manager library (libgeniex.so)
# Headers (geniex.h, geniex_model.h) and the .so are staged into
# /usr/include and /usr/lib by the Dockerfile builder stage. GENIEX_LIB_PATH
# may be set to override the search location for local builds.
# ─────────────────────────────────────────────────────────────────────────────
find_library(GENIEX_LIB
    NAMES geniex libgeniex
    PATHS ${GENIEX_LIB_PATH} /usr/lib
)
if(NOT GENIEX_LIB)
    message(FATAL_ERROR "libgeniex.so not found (set GENIEX_LIB_PATH or stage it into /usr/lib)")
endif()

# Built-in Predictive AI postprocessing plugins (built as standalone .so
# files, loaded at runtime by PostprocPlugin — see
# postprocess-plugins/CMakeLists.txt for how to add a new one). Not linked
# into qaiserve itself.
add_subdirectory(postprocess-plugins)

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
    src/admin/GenieXClient.cpp
    src/admin/ModelFetchJob.cpp

    # OIP v2 — model discovery (GET /v1/models, GET /v2/models, GET /v2/models/{model})
    src/controllers/ModelsController.cpp

    # OIP v2 — inference (POST /v2/models/{model}/infer|generate|generate_stream)
    # InferController.cpp already listed above; add OIP parsing helpers
    src/oip/OipDTOs.cpp
    src/oip/OipBinaryParser.cpp
    src/oip/MultipartParser.cpp

    # Chat Completions endpoint (OpenAI-compatible API)
    src/ChatCompletionUtils.cpp
    src/ChatCompletionStore.cpp
    src/controllers/ChatCompletionsController.cpp

    # Shared utilities (used by HTTP + WebSocket controllers)
    src/ResponsesUtils.cpp
    src/ResponseStore.cpp
    src/ResponsesCompactionService.cpp
    src/TokenBudgetUtils.cpp

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

    # gRPC layer — ChatService (unary + streaming) and InferService (predictive
    # tensor inference), same process/model pool as HTTP/WS, own listener port
    # (GRPC_SERVER_PORT, default 50051).
    src/grpc/ChatServiceImpl.cpp
    src/grpc/InferServiceImpl.cpp
    src/grpc/GrpcErrorMapping.cpp
    ${PROTO_GEN_SRC}
    ${GRPC_GEN_SRC}
    
    # Postprocess extension interface, Runtime loading, Registry, ABI conversion
    src/postproc/PostprocRegistry.cpp
    src/postproc/PostprocPlugin.cpp
    src/postproc/PostprocAbiConvert.cpp

)

add_executable(qaiserve ${SERVER_SOURCES})
add_dependencies(qaiserve qai_forge_proto_gen)

# ─────────────────────────────────────────────────────────────────────────────
# Include directories
# ─────────────────────────────────────────────────────────────────────────────
target_include_directories(qaiserve PRIVATE
    # QAIServe own headers (controllers/, mcp/, ws/, tools/, ResponsesUtils.h, admin/, grpc/)
    ${CMAKE_CURRENT_SOURCE_DIR}/include

    # qai-forge SDK headers (sibling directory)
    ${CMAKE_CURRENT_SOURCE_DIR}/../qai-forge/include

    # Generated protobuf/gRPC stubs
    ${GENERATED_DIR}
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
    gRPC::grpc++
    protobuf::libprotobuf
    ${GENIEX_LIB}
    OpenSSL::Crypto
    # dlopen/dlsym/dlclose for PostprocRegistry's runtime plugin loading.
    ${CMAKE_DL_LIBS}
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
