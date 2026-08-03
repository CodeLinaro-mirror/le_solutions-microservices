// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <drogon/drogon.h>
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/PrivilegeDrop.h"
#include "qai_forge/QaiForge.h"
#include "mcp/McpClientRegistry.h"
#include "mcp/NativeToolRegistry.h"
#include "postproc/PostprocRegistry.h"
#include "tools/DateTimeTool.h"
#include "tools/CalculatorTool.h"
#include "grpc/ChatServiceImpl.h"
#include "grpc/InferServiceImpl.h"
#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

// Default postprocess plugins dir, overridable via env vars
static const char* kDefaultBuiltinPluginsDir = "/usr/lib/postproc_plugins";
static const char* kDefaultClientPluginsDir = "/mnt/work/plugins";

int main() {
    // Step 1: Drop privileges before any threads or sockets are opened.
    // Discovers host GIDs for DSP/DMA devices, sets 0666 permissions on them,
    // deploys the fastrpc DSP config, and drops from root to the models-dir owner.
    // Child processes (genai-inference-worker, MCP servers, qnn/snpe workers) will inherit these privileges.
    qai_forge::utils::drop_privileges_and_bind_devices();

    // Step 2: Read port from environment variable (default 9002)
    const char* port_env = std::getenv("QAISERVE_PORT");
    if (!port_env) port_env = std::getenv("RESPONSES_PORT"); // fallback
    int port = port_env ? std::stoi(port_env) : 9002;

    // Read max body size from env (bytes). Default 16 MB.
    // Set higher for models with large input tensors.
    const char* body_size_env = std::getenv("QAISERVE_MAX_BODY_SIZE");
    size_t max_body_size = body_size_env ? std::stoull(body_size_env) : 16ULL * 1024 * 1024;

    // Log admin token status (never log the token value itself)
    const char* admin_token = std::getenv("QAISERVE_ADMIN_TOKEN");
    if (admin_token && admin_token[0] != '\0') {
        std::cout << "[main] Admin model management: ENABLED (X-Admin-Token required)" << std::endl;
    } else {
        std::cout << "[main] Admin model management: DISABLED "
                  << "(set QAISERVE_ADMIN_TOKEN to enable /admin/models/* endpoints)" << std::endl;
    }

    std::cout << "[main] Starting QAIServe unified inference server on port " << port
              << "  max_body=" << (max_body_size) << "B" << std::endl;

    // Read gRPC listener port from environment variable (default 50051).
    const char* grpc_port_env = std::getenv("GRPC_SERVER_PORT");
    int grpc_port = grpc_port_env ? std::stoi(grpc_port_env) : 50051;

    // gRPC server + ChatService live for the duration of main() — started
    // inside registerBeginningAdvice (once Drogon's event loop is up) and
    // torn down after drogon::app()....run() returns below. Declared here,
    // not inside the advice lambda, so they outlive that one-shot callback.
    ChatServiceImpl chat_service;
    InferServiceImpl infer_service;
    std::unique_ptr<grpc::Server> grpc_server;
    std::thread grpc_thread;

    // Step 3: Configure and run the Drogon HTTP server
    // Log to stdout — standard practice for containerised services.
    drogon::app()
        .setLogLevel(trantor::Logger::kInfo)
        .addListener("0.0.0.0", port)
        .setThreadNum(4)
        .setMaxConnectionNum(1000)
        .setClientMaxBodySize(max_body_size)
        .registerBeginningAdvice([&]() {
            // ── Step 3a: Scan model bundles ───────────────────────────────────
            // ModelConfigManager is a lazy singleton — validateModel() always
            // returns false until scanModelBundles() is called at least once.
            ModelConfigManager::getInstance().scanModelBundles();
            std::cout << "[main] Model bundles scanned." << std::endl;

            // ── Step 3a-2: Load postprocess plugins ─────────────────────────
            // Built-in plugins first (baked into the image), then any client-supplied
            // plugins mounted at POSTPROC_PLUGINS_DIR — a client plugin whose
            // name collides with a built-in is skipped.
            auto& postproc_registry = PostprocRegistry::getInstance();
            const char* builtin_env = std::getenv("POSTPROC_BUILTIN_PLUGINS_DIR");
            const std::string builtin_dir =
                (builtin_env && builtin_env[0] != '\0')
                    ? builtin_env
                    : kDefaultBuiltinPluginsDir;
            postproc_registry.loadDirectory(builtin_dir);
            const char* client_env = std::getenv("POSTPROC_PLUGINS_DIR");
            const std::string client_dir =
                (client_env && client_env[0] != '\0')
                    ? client_env
                    : kDefaultClientPluginsDir;
            postproc_registry.loadDirectory(client_dir);

            qai_forge::QaiForge::getInstance().start();
            std::cout << "[main] Inference engine started." << std::endl;

            // ── Step 3b: Register built-in native tools ───────────────────────
            // Native tools run in-process (no subprocess, no network).
            // They are automatically injected into every request so the model
            // always knows what built-in capabilities are available.
            //
            // To add a new native tool:
            //   1. Implement ITool in include/tools/MyTool.h + src/tools/MyTool.cpp
            //   2. Add one line below: native_registry.registerTool(...)
            //   3. Add the source file to CMakeLists.txt SERVER_SOURCES
            auto& native_registry = NativeToolRegistry::getInstance();
            native_registry.registerTool(std::make_unique<DateTimeTool>());
            native_registry.registerTool(std::make_unique<CalculatorTool>());
            std::cout << "[main] Registered " << native_registry.toolCount()
                      << " native tool(s): ";
            for (const auto& t : native_registry.listTools()) {
                std::cout << t.name << " ";
            }
            std::cout << std::endl;

            // ── Step 3c: Load and connect MCP servers ─────────────────────────
            // Determine config file path:
            //   1. QAISERVE_MCP_CONFIG or RESPONSES_MCP_CONFIG env var (explicit path)
            //   2. ./mcp_servers.json (same directory as binary)
            //   3. /etc/QAIServe/mcp_servers.json (system-wide config)
            auto& mcp_registry = McpClientRegistry::getInstance();

            // Always register the "native" virtual server.
            {
                McpServerConfig native_cfg;
                native_cfg.name      = "native";
                native_cfg.transport = "native";
                mcp_registry.registerServer(native_cfg);
            }

            const char* mcp_config_env = std::getenv("QAISERVE_MCP_CONFIG");
            if (!mcp_config_env) mcp_config_env = std::getenv("RESPONSES_MCP_CONFIG");

            std::string mcp_config_path;

            if (mcp_config_env && mcp_config_env[0] != '\0') {
                mcp_config_path = mcp_config_env;
            } else {
                // Try local path first
                mcp_config_path = "./mcp_servers.json";
            }

            mcp_registry.loadConfig(mcp_config_path);

            if (mcp_registry.serverCount() > 0) {
                std::cout << "[main] Connecting " << mcp_registry.serverCount()
                          << " MCP server(s)..." << std::endl;
                mcp_registry.connectAll();
                std::cout << "[main] MCP servers connected." << std::endl;
            } else {
                std::cout << "[main] No MCP servers configured "
                          << "(set QAISERVE_MCP_CONFIG or create ./mcp_servers.json "
                          << "to enable MCP tool support)." << std::endl;
            }

            // ── Step 3d: Build and start the gRPC ChatService server ──────────
            // Runs on its own background thread since grpc::Server::Wait() is
            // a blocking call, same shape as drogon::app()....run() below —
            // rather than intertwine the two event loops, each blocking call
            // gets its own thread. Shares the same QaiForge singleton/worker
            // pool as the HTTP/WS transport started above.
            grpc::ServerBuilder builder;
            std::string grpc_address = "0.0.0.0:" + std::to_string(grpc_port);
            builder.AddListeningPort(grpc_address, grpc::InsecureServerCredentials());
            builder.RegisterService(&chat_service);
            builder.RegisterService(&infer_service);

            // gRPC's built-in default max message size is 4 MiB, both for
            // messages this server sends and receives — predictive model
            // outputs (e.g. detection/segmentation tensors) can exceed that.
            // Raise both limits; a client-side receive-limit increase alone
            // cannot help, since the server would refuse to send an
            // oversized response before it ever reaches the client.
            const char* grpc_max_msg_env = std::getenv("QAISERVE_GRPC_MAX_MESSAGE_SIZE");
            int grpc_max_message_size = grpc_max_msg_env ? std::stoi(grpc_max_msg_env) : 64 * 1024 * 1024;
            builder.SetMaxSendMessageSize(grpc_max_message_size);
            builder.SetMaxReceiveMessageSize(grpc_max_message_size);
            grpc_server = builder.BuildAndStart();
            grpc_thread = std::thread([&grpc_server]() { grpc_server->Wait(); });
            std::cout << "[main] gRPC ChatService + InferService server listening on " << grpc_address << std::endl;

            std::cout << "[main] QAIServe server started." << std::endl;
        })
        .run();

    // Cleanup: shut down the gRPC server first, then the inference engine,
    // then disconnect MCP servers.
    if (grpc_server) {
        grpc_server->Shutdown();
    }
    if (grpc_thread.joinable()) {
        grpc_thread.join();
    }
    qai_forge::QaiForge::getInstance().shutdown();
    McpClientRegistry::getInstance().disconnectAll();

    return 0;
}
