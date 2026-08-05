// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// WsResponsesController.cpp — WebSocket Responses API controller implementation
//
// Endpoint: ws://host:9002/v1/responses/ws
//
// Runs inside the same Drogon instance as the HTTP ResponsesController.
// Drogon handles the HTTP→WebSocket upgrade automatically on port 9002.
//
// Threading model:
//   - handleNewConnection / handleNewMessage / handleConnectionClosed run on
//     Drogon's event loop threads (thread pool)
//   - runResponse() is spawned as a detached std::thread to avoid blocking
//     the event loop during inference (which can take seconds)
//   - sendEvent() is thread-safe (Drogon's conn->send() is thread-safe)
//   - states_ map is protected by shared_mutex
// ─────────────────────────────────────────────────────────────────────────────

#include "ws/WsResponsesController.h"
#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/session/SessionManager.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>
#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string generate_connection_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "wsconn_" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// sendEvent — send a JSON event as a WebSocket text frame
// Thread-safe: Drogon's conn->send() is thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::sendEvent(const drogon::WebSocketConnectionPtr& conn,
                                       const json& event) {
    if (!conn || conn->disconnected()) return;
    try {
        conn->send(event.dump());
    } catch (const std::exception& e) {
        std::cerr << "[WsResponsesController] sendEvent failed: " << e.what() << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// getConnectionId — retrieve the connection ID from the connection context
// ─────────────────────────────────────────────────────────────────────────────
std::string WsResponsesController::getConnectionId(
    const drogon::WebSocketConnectionPtr& conn) {
    auto ctx = conn->getContext<std::string>();
    if (!ctx) return "";
    return *ctx;
}

// ─────────────────────────────────────────────────────────────────────────────
// handleNewConnection — called by Drogon after HTTP Upgrade completes
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::handleNewConnection(
    const drogon::HttpRequestPtr& req,
    const drogon::WebSocketConnectionPtr& conn) {

    std::string conn_id = generate_connection_id();
    conn->setContext(std::make_shared<std::string>(conn_id));

    WsConnectionState state;
    state.connection_id = conn_id;
    state.connected_at  = std::chrono::steady_clock::now();

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        states_[conn_id] = std::move(state);
    }

    std::cout << "[WsResponsesController] New connection: " << conn_id
              << " from " << req->getPeerAddr().toIpPort() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// handleNewMessage — called by Drogon for each incoming WS frame
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::handleNewMessage(
    const drogon::WebSocketConnectionPtr& conn,
    std::string&& message,
    const drogon::WebSocketMessageType& type) {

    // Ignore ping/pong/close frames
    if (type == drogon::WebSocketMessageType::Ping) {
        conn->send("", drogon::WebSocketMessageType::Pong);
        return;
    }
    if (type != drogon::WebSocketMessageType::Text) return;

    std::string conn_id = getConnectionId(conn);
    if (conn_id.empty()) return;

    // ── Check connection timeout ──────────────────────────────────────────────
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(conn_id);
        if (it == states_.end()) return;
        if (it->second.isExpired()) {
            lock.unlock();
            int timeout_min = WsProtocol::connection_timeout_minutes();
            sendEvent(conn, WsProtocol::make_error(400,
                WsProtocol::ERR_CONNECTION_LIMIT_REACHED,
                "Responses websocket connection limit reached (" +
                std::to_string(timeout_min) +
                " minutes). Create a new websocket connection to continue."));
            conn->shutdown();
            return;
        }
    }

    // ── Parse JSON ────────────────────────────────────────────────────────────
    json event;
    try {
        event = json::parse(message);
    } catch (...) {
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            "Invalid JSON message"));
        return;
    }

    std::string event_type = event.value("type", "");

    // ── Dispatch ──────────────────────────────────────────────────────────────
    if (event_type == WsProtocol::CLIENT_RESPONSE_CREATE) {
        // Acquire exclusive lock once, get a reference, then release before
        // calling onResponseCreate (which may itself acquire the lock).
        // We hold the lock only long enough to verify the state exists.
        {
            std::shared_lock<std::shared_mutex> check_lock(mutex_);
            if (states_.find(conn_id) == states_.end()) return;
        }

        // onResponseCreate takes a reference to the state — the caller must
        // hold the exclusive lock while accessing it. We pass the reference
        // and let onResponseCreate manage its own locking internally.
        std::unique_lock<std::shared_mutex> wlock(mutex_);
        auto it = states_.find(conn_id);
        if (it == states_.end()) return;
        WsConnectionState& state = it->second;
        wlock.unlock();

        onResponseCreate(conn, state, event);
    } else {
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_UNKNOWN_EVENT_TYPE,
            "Unknown event type: '" + event_type + "'. "
            "Only 'response.create' is supported."));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handleConnectionClosed — called by Drogon when the connection closes
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& conn) {

    std::string conn_id = getConnectionId(conn);
    if (conn_id.empty()) return;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        states_.erase(conn_id);
    }

    std::cout << "[WsResponsesController] Connection closed: " << conn_id << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// onResponseCreate — handle "response.create" event
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::onResponseCreate(
    const drogon::WebSocketConnectionPtr& conn,
    WsConnectionState& state,
    const json& event) {

    // ── Sequential enforcement ────────────────────────────────────────────────
    if (state.response_in_flight.exchange(true)) {
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_RESPONSE_IN_PROGRESS,
            "A response is already in progress on this connection. "
            "Wait for response.completed before sending another response.create."));
        return;
    }

    // ── Parse required fields ─────────────────────────────────────────────────
    if (!event.contains("model") || !event.contains("input")) {
        state.response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            "Missing required fields: 'model' and 'input'"));
        return;
    }

    std::string model              = event.value("model", "");
    std::string system_prompt      = event.value("instructions", "");
    std::string previous_response_id = event.value("previous_response_id", "");
    bool        store_flag         = event.value("store", true);
    bool        generate           = event.value("generate", true);

    // Update store flag on the connection state
    state.store = store_flag;

    // ── Validate model ────────────────────────────────────────────────────────
    if (!ModelConfigManager::getInstance().validateModel(model)) {
        state.response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(404,
            WsProtocol::ERR_MODEL_NOT_FOUND,
            "Model '" + model + "' not found. Check /v1/models for available models."));
        return;
    }

    // ── Resolve session from previous_response_id ─────────────────────────────
    std::string resolved_session_id;
    if (!previous_response_id.empty()) {
        resolved_session_id = state.resolveSession(previous_response_id);
        if (resolved_session_id.empty()) {
            state.response_in_flight.store(false);
            sendEvent(conn, WsProtocol::make_error(400,
                WsProtocol::ERR_PREVIOUS_RESPONSE_NOT_FOUND,
                "Previous response with id '" + previous_response_id + "' not found.",
                "previous_response_id"));
            return;
        }
    }

    // ── Parse tools ───────────────────────────────────────────────────────────
    bool has_mcp_tools      = false;
    bool has_function_tools = false;
    json function_tools     = json::array();
    std::vector<McpToolRequest> mcp_requests;

    if (event.contains("tools") && event["tools"].is_array()) {
        for (const auto& tool : event["tools"]) {
            std::string tool_type = tool.value("type", "");
            if (tool_type == "mcp") {
                has_mcp_tools = true;
            } else if (tool_type == "function") {
                has_function_tools = true;
                function_tools.push_back(tool);
            } else if (tool_type == "web_search" || tool_type == "file_search"
                       || tool_type == "code_interpreter") {
                state.response_in_flight.store(false);
                sendEvent(conn, WsProtocol::make_error(400,
                    WsProtocol::ERR_INVALID_REQUEST,
                    "Built-in tool '" + tool_type + "' is not available on-device. "
                    "Use 'function' or 'mcp' tool types."));
                return;
            }
        }
        if (has_mcp_tools) {
            mcp_requests = ResponsesUtils::extract_mcp_tool_requests(event["tools"]);
        }
    }

    // ── Validate MCP servers ──────────────────────────────────────────────────
    if (has_mcp_tools) {
        auto& registry = McpClientRegistry::getInstance();
        for (const auto& mcp_req : mcp_requests) {
            if (!mcp_req.server_label.empty() && !registry.hasServer(mcp_req.server_label)) {
                state.response_in_flight.store(false);
                sendEvent(conn, WsProtocol::make_error(400,
                    WsProtocol::ERR_MCP_SERVER_NOT_FOUND,
                    "MCP server '" + mcp_req.server_label + "' is not registered. "
                    "Check mcp_servers.json configuration."));
                return;
            }
        }
    }

    // ── Convert input to messages ─────────────────────────────────────────────
    json messages = ResponsesUtils::input_to_messages(event["input"], system_prompt);
    if (messages.empty()) {
        state.response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            "Input produced no messages"));
        return;
    }

    // ── Build SDK request ─────────────────────────────────────────────────────
    CreateChatCompletionRequest sdk_request;
    try {
        json sdk_body = {
            {"model",    model},
            {"messages", messages},
            {"stream",   false}
        };
        if (event.contains("max_output_tokens") && !event["max_output_tokens"].is_null())
            sdk_body["max_completion_tokens"] = event["max_output_tokens"];
        if (event.contains("temperature") && !event["temperature"].is_null())
            sdk_body["temperature"] = event["temperature"];
        if (event.contains("top_p") && !event["top_p"].is_null())
            sdk_body["top_p"] = event["top_p"];

        // Use resolved session ID (or response_id as session key for new sessions)
        std::string session_key = resolved_session_id.empty()
            ? ResponsesUtils::generate_response_id()
            : resolved_session_id;
        sdk_body["user"] = session_key;

        sdk_request = CreateChatCompletionRequest::from_json(sdk_body);
    } catch (const std::exception& e) {
        state.response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            std::string("Request parsing error: ") + e.what()));
        return;
    }

    // ── Aggregate MCP tools ───────────────────────────────────────────────────
    json mcp_function_tools = json::array();
    if (has_mcp_tools) {
        auto& registry = McpClientRegistry::getInstance();
        for (const auto& mcp_req : mcp_requests) {
            json server_tools = registry.getAllTools(mcp_req.server_label,
                                                      mcp_req.allowed_tools);
            for (const auto& t : server_tools) mcp_function_tools.push_back(t);
        }
    }
    for (const auto& ft : function_tools) mcp_function_tools.push_back(ft);

    std::string response_id    = ResponsesUtils::generate_response_id();
    std::string new_session_id = sdk_request.user.value_or(response_id);

    // ── generate=false warmup path ────────────────────────────────────────────
    if (!generate) {
        runWarmup(conn, state, sdk_request, response_id, model, new_session_id);
        return;
    }

    // ── Spawn inference thread ────────────────────────────────────────────────
    // Capture by value — conn is a shared_ptr, everything else is copied.
    std::thread([this, conn, conn_id = state.connection_id,
                 sdk_request, mcp_function_tools,
                 has_mcp_tools, response_id, model, new_session_id]() mutable {
        runResponse(conn, conn_id, sdk_request, mcp_function_tools,
                    has_mcp_tools, response_id, model, new_session_id);
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// runWarmup — generate=false: pre-load session without inference
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::runWarmup(
    const drogon::WebSocketConnectionPtr& conn,
    WsConnectionState& state,
    const CreateChatCompletionRequest& sdk_request,
    const std::string& response_id,
    const std::string& model,
    const std::string& new_session_id) {

    // Pre-warm the session in SessionManager
    auto& session_mgr = SessionManager::getInstance();
    session_mgr.findOrCreate(new_session_id, "ws_warmup");

    int created_time = ResponsesUtils::current_unix_time();

    // Send response.created
    sendEvent(conn, {
        {"type", WsProtocol::SERVER_RESPONSE_CREATED},
        {"response", {
            {"id",         response_id},
            {"object",     "response"},
            {"created_at", created_time},
            {"model",      model},
            {"status",     "completed"},
            {"output",     json::array()}
        }}
    });

    // Send response.completed (empty output — warmup only)
    sendEvent(conn, {
        {"type", WsProtocol::SERVER_RESPONSE_COMPLETED},
        {"response", ResponsesUtils::build_response_object(
            response_id, model, json::array(), "completed", 0, 0,
            created_time, json(nullptr), json(nullptr))}
    });

    // Update connection-local cache
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(state.connection_id);
        if (it != states_.end()) {
            it->second.last_response_id = response_id;
            it->second.last_session_id  = new_session_id;
        }
    }

    state.response_in_flight.store(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// runResponse — full inference execution on a detached thread
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::runResponse(
    drogon::WebSocketConnectionPtr conn,
    std::string connection_id,
    CreateChatCompletionRequest sdk_request,
    json mcp_function_tools,
    bool has_mcp_tools,
    std::string response_id,
    std::string model,
    std::string new_session_id) {

    int created_time = ResponsesUtils::current_unix_time();

    // ── Step 1: response.created ──────────────────────────────────────────────
    sendEvent(conn, {
        {"type", WsProtocol::SERVER_RESPONSE_CREATED},
        {"response", {
            {"id",         response_id},
            {"object",     "response"},
            {"created_at", created_time},
            {"model",      model},
            {"status",     "in_progress"},
            {"output",     json::array()}
        }}
    });

    // ── Step 2: response.output_item.added ────────────────────────────────────
    sendEvent(conn, {
        {"type",         WsProtocol::SERVER_OUTPUT_ITEM_ADDED},
        {"output_index", 0},
        {"item", {
            {"type",    "message"},
            {"id",      "msg_" + response_id},
            {"role",    "assistant"},
            {"content", json::array()},
            {"status",  "in_progress"}
        }}
    });

    // ── Step 3: Run inference ─────────────────────────────────────────────────
    bool had_error = false;
    std::string error_msg;
    std::vector<McpCallRecord> mcp_records;
    StandardResponse final_response;

    auto& orchestrator = ChatOrchestrator::getInstance();

    if (has_mcp_tools && !mcp_function_tools.empty()) {
        // ── MCP path: McpAgenticLoop::runStreaming() ──────────────────────────
        const char* max_iter_env = std::getenv("RESPONSES_MCP_MAX_ITERATIONS");
        int max_iterations = max_iter_env ? std::stoi(max_iter_env) : 10;

        try {
            auto& registry = McpClientRegistry::getInstance();
            McpAgenticLoop loop(registry, max_iterations);

            // WS emitter — sends MCP events directly as WS text frames
            McpSseEmitter ws_emitter = [&conn](const std::string& /*event_type*/,
                                                const json& data) {
                sendEvent(conn, data);
            };

            McpLoopResult loop_result = loop.runStreaming(
                sdk_request, mcp_function_tools, ws_emitter, response_id);

            final_response = loop_result.final_response;
            mcp_records    = loop_result.call_records;

        } catch (const GenAIException& e) {
            had_error = true;
            error_msg = e.message;
        } catch (const std::exception& e) {
            had_error = true;
            error_msg = e.what();
        }

    } else {
        // ── Standard path: ChatOrchestrator::handleStreaming() ────────────────
        // Emit content_part.added before streaming tokens
        sendEvent(conn, {
            {"type",          WsProtocol::SERVER_CONTENT_PART_ADDED},
            {"output_index",  0},
            {"content_index", 0},
            {"part",          {{"type", "output_text"}, {"text", ""}}}
        });

        std::string full_text;
        sdk_request.stream = true;

        try {
            orchestrator.handleStreaming(sdk_request,
                [&conn, &full_text, &response_id](const StreamChunk& chunk) {
                    if (chunk.content_delta.has_value()
                        && !chunk.content_delta.value().empty()) {
                        full_text += chunk.content_delta.value();
                        sendEvent(conn, {
                            {"type",          WsProtocol::SERVER_OUTPUT_TEXT_DELTA},
                            {"output_index",  0},
                            {"content_index", 0},
                            {"delta",         chunk.content_delta.value()}
                        });
                    }
                    if (chunk.reasoning_content.has_value()
                        && !chunk.reasoning_content.value().empty()) {
                        sendEvent(conn, {
                            {"type",         WsProtocol::SERVER_REASONING_DELTA},
                            {"output_index", 0},
                            {"delta",        chunk.reasoning_content.value()}
                        });
                    }
                });
        } catch (const GenAIException& e) {
            had_error = true;
            error_msg = e.message;
        } catch (const std::exception& e) {
            had_error = true;
            error_msg = e.what();
        }

        if (!had_error) {
            final_response.content = full_text;
            final_response.id      = new_session_id;
            final_response.model   = model;

            // response.output_text.done
            sendEvent(conn, {
                {"type",          WsProtocol::SERVER_OUTPUT_TEXT_DONE},
                {"output_index",  0},
                {"content_index", 0},
                {"text",          full_text}
            });
        }
    }

    // ── Step 4: Error or completion ───────────────────────────────────────────
    if (had_error) {
        sendEvent(conn, WsProtocol::make_error(500,
            WsProtocol::ERR_INFERENCE_FAILED, error_msg));

        // Release in-flight flag even on error
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(connection_id);
        if (it != states_.end()) {
            it->second.response_in_flight.store(false);
        }
        return;
    }

    // response.output_item.done
    std::string final_text = final_response.content.value_or("");
    int output_index = static_cast<int>(mcp_records.size());

    sendEvent(conn, {
        {"type",         WsProtocol::SERVER_OUTPUT_ITEM_DONE},
        {"output_index", output_index},
        {"item", {
            {"type",    "message"},
            {"id",      "msg_" + response_id},
            {"role",    "assistant"},
            {"content", {{{"type", "output_text"}, {"text", final_text}}}},
            {"status",  "completed"}
        }}
    });

    // response.completed
    json output = ResponsesUtils::build_output_array(final_response, mcp_records);
    sendEvent(conn, {
        {"type", WsProtocol::SERVER_RESPONSE_COMPLETED},
        {"response", ResponsesUtils::build_response_object(
            response_id, model, output, "completed",
            final_response.prompt_tokens,
            final_response.completion_tokens,
            created_time, json(nullptr), json(nullptr))}
    });

    // ── Step 5: Update connection-local cache ─────────────────────────────────
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(connection_id);
        if (it != states_.end()) {
            it->second.last_response_id = response_id;
            it->second.last_session_id  = new_session_id;
            it->second.response_in_flight.store(false);
        }
    }
}
