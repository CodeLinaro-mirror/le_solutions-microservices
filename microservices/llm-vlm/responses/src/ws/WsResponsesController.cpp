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
#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <algorithm>
#include <future>
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

static json append_messages(const json& prefix, const json& suffix) {
    json messages = json::array();
    for (const auto& message : prefix) {
        messages.push_back(message);
    }
    for (const auto& message : suffix) {
        messages.push_back(message);
    }
    return messages;
}

static json make_assistant_message(const StandardResponse& response) {
    json message = {
        {"role", "assistant"},
        {"content", response.content.value_or("")},
    };
    if (response.reasoning_content.has_value() &&
        !response.reasoning_content->empty()) {
        message["_thinking_content"] = response.reasoning_content.value();
    }
    if (response.tool_calls.has_value() && !response.tool_calls->empty()) {
        message["tool_calls"] = response.tool_calls.value();
    }
    return message;
}

static qai_forge::GenerateOptions make_generate_options(
    const WsConnectionState& state,
    const std::string& response_id,
    const std::string& memory_parent_turn_id,
    const std::string& tool_chain_response_id,
    bool tool_output_submission) {
    qai_forge::GenerateOptions options;
    options.response_id = response_id;
    options.session_id = state.connection_id;

    qai_forge::ConversationReference reference;
    reference.namespace_id = "responses.websocket";
    reference.conversation_id = state.connection_id;
    reference.turn_id = response_id;
    if (memory_parent_turn_id.empty()) {
        reference.parent_policy =
            qai_forge::ConversationParentPolicy::Root;
    } else {
        reference.parent_policy =
            qai_forge::ConversationParentPolicy::Explicit;
        reference.parent_turn_id = memory_parent_turn_id;
    }
    options.conversation = std::move(reference);

    if (tool_output_submission && !tool_chain_response_id.empty()) {
        options.previous_response_id = tool_chain_response_id;
        options.tool_output_submission = true;
        options.allow_tool_chain_fallback = true;
    }
    return options;
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

    auto state = std::make_shared<WsConnectionState>();
    state->connection_id = conn_id;
    state->connected_at  = std::chrono::steady_clock::now();

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
        if (it->second->isExpired()) {
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
        std::shared_ptr<WsConnectionState> state = it->second;
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

    std::shared_ptr<WsConnectionState> state;
    std::string active_response_id;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto found = states_.find(conn_id);
        if (found != states_.end()) {
            state = found->second;
            active_response_id = state->active_response_id;
            states_.erase(found);
        }
    }
    if (state) {
        if (!active_response_id.empty()) {
            qai_forge::QaiForge::getInstance().cancel(
                active_response_id);
        }
        qai_forge::QaiForge::getInstance().releaseConversation(
            "responses.websocket", state->connection_id);
    }

    std::cout << "[WsResponsesController] Connection closed: " << conn_id << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// onResponseCreate — handle "response.create" event
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::onResponseCreate(
    const drogon::WebSocketConnectionPtr& conn,
    const std::shared_ptr<WsConnectionState>& state,
    const json& event) {
    if (state->response_in_flight.exchange(true)) {
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_RESPONSE_IN_PROGRESS,
            "A response is already in progress on this connection. "
            "Wait for response.completed before sending another response.create."));
        return;
    }

    if (!event.contains("model") || !event.contains("input")) {
        state->response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            "Missing required fields: 'model' and 'input'"));
        return;
    }

    const std::string model = event.value("model", "");
    const std::string system_prompt = event.value("instructions", "");
    const std::string previous_response_id =
        event.value("previous_response_id", "");
    const bool generate = event.value("generate", true);

    if (!ModelConfigManager::getInstance().validateModel(model)) {
        state->response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(404,
            WsProtocol::ERR_MODEL_NOT_FOUND,
            "Model '" + model + "' not found. Check /v1/models for available models."));
        return;
    }

    json ancestor_messages = json::array();
    std::string memory_parent_turn_id;
    std::string tool_chain_response_id;
    bool retained_tool_output = false;
    if (!previous_response_id.empty()) {
        std::optional<WsStoredResponse> parent;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            parent = state->findResponse(previous_response_id);
        }
        if (!parent.has_value()) {
            state->response_in_flight.store(false);
            sendEvent(conn, WsProtocol::make_error(400,
                WsProtocol::ERR_PREVIOUS_RESPONSE_NOT_FOUND,
                "Previous response with id '" + previous_response_id + "' not found.",
                "previous_response_id"));
            return;
        }
        ancestor_messages = std::move(parent->messages);
        memory_parent_turn_id = std::move(parent->memory_turn_id);
        tool_chain_response_id = std::move(parent->tool_chain_response_id);
        retained_tool_output = parent->tool_output_pending;
    }

    bool has_mcp_tools = false;
    json function_tools = json::array();
    std::vector<McpToolRequest> mcp_requests;
    if (event.contains("tools") && event["tools"].is_array()) {
        for (const auto& tool : event["tools"]) {
            const std::string tool_type = tool.value("type", "");
            if (tool_type == "mcp") {
                has_mcp_tools = true;
            } else if (tool_type == "function") {
                function_tools.push_back(tool);
            } else if (tool_type == "web_search" || tool_type == "file_search" ||
                       tool_type == "code_interpreter") {
                state->response_in_flight.store(false);
                sendEvent(conn, WsProtocol::make_error(400,
                    WsProtocol::ERR_INVALID_REQUEST,
                    "Built-in tool '" + tool_type + "' is not available on-device. "
                    "Use 'function' or 'mcp' tool types."));
                return;
            }
        }
        if (has_mcp_tools) {
            mcp_requests =
                ResponsesUtils::extract_mcp_tool_requests(event["tools"]);
        }
    }

    if (has_mcp_tools) {
        auto& registry = McpClientRegistry::getInstance();
        for (const auto& mcp_request : mcp_requests) {
            if (!mcp_request.server_label.empty() &&
                !registry.hasServer(mcp_request.server_label)) {
                state->response_in_flight.store(false);
                sendEvent(conn, WsProtocol::make_error(400,
                    WsProtocol::ERR_MCP_SERVER_NOT_FOUND,
                    "MCP server '" + mcp_request.server_label +
                        "' is not registered. Check mcp_servers.json configuration."));
                return;
            }
        }
    }

    json current_messages =
        ResponsesUtils::input_to_messages(event["input"], system_prompt);
    if (current_messages.empty()) {
        state->response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            "Input produced no messages"));
        return;
    }
    const bool current_has_tool_output = std::any_of(
        current_messages.begin(),
        current_messages.end(),
        [](const json& message) {
            return message.is_object() && message.value("role", "") == "tool";
        });
    const bool tool_output_submission =
        retained_tool_output || current_has_tool_output;
    if (current_has_tool_output && tool_chain_response_id.empty()) {
        tool_chain_response_id = previous_response_id;
    }
    json messages = append_messages(ancestor_messages, current_messages);
    const std::string response_id = ResponsesUtils::generate_response_id();

    CreateChatCompletionRequest sdk_request;
    try {
        json sdk_body = {
            {"model", model},
            {"messages", messages},
            {"stream", false},
            {"user", state->connection_id},
        };
        if (event.contains("max_output_tokens") &&
            !event["max_output_tokens"].is_null()) {
            sdk_body["max_completion_tokens"] = event["max_output_tokens"];
        }
        if (event.contains("temperature") && !event["temperature"].is_null()) {
            sdk_body["temperature"] = event["temperature"];
        }
        if (event.contains("top_p") && !event["top_p"].is_null()) {
            sdk_body["top_p"] = event["top_p"];
        }
        sdk_request = CreateChatCompletionRequest::from_json(sdk_body);
    } catch (const std::exception& error) {
        state->response_in_flight.store(false);
        sendEvent(conn, WsProtocol::make_error(400,
            WsProtocol::ERR_INVALID_REQUEST,
            std::string("Request parsing error: ") + error.what()));
        return;
    }

    json mcp_function_tools = json::array();
    if (has_mcp_tools) {
        auto& registry = McpClientRegistry::getInstance();
        for (const auto& mcp_request : mcp_requests) {
            const json server_tools = registry.getAllTools(
                mcp_request.server_label, mcp_request.allowed_tools);
            for (const auto& tool : server_tools) {
                mcp_function_tools.push_back(tool);
            }
        }
    }
    for (const auto& tool : function_tools) {
        mcp_function_tools.push_back(tool);
    }
    if (!mcp_function_tools.empty()) {
        sdk_request.tools = mcp_function_tools;
    }

    if (!generate) {
        runWarmup(
            conn,
            state,
            std::move(messages),
            response_id,
            model,
            std::move(memory_parent_turn_id),
            std::move(tool_chain_response_id),
            tool_output_submission);
        return;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto found = states_.find(state->connection_id);
        if (found == states_.end() || found->second != state) {
            state->response_in_flight.store(false);
            return;
        }
        state->active_response_id = response_id;
    }

    std::thread([this, conn, state, sdk_request, mcp_function_tools,
                 has_mcp_tools, response_id, model, memory_parent_turn_id,
                 tool_chain_response_id, tool_output_submission]() mutable {
        runResponse(
            conn,
            state,
            std::move(sdk_request),
            std::move(mcp_function_tools),
            has_mcp_tools,
            std::move(response_id),
            std::move(model),
            std::move(memory_parent_turn_id),
            std::move(tool_chain_response_id),
            tool_output_submission);
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────────
// runWarmup — generate=false: retain input without inference
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::runWarmup(
    const drogon::WebSocketConnectionPtr& conn,
    const std::shared_ptr<WsConnectionState>& state,
    json messages,
    const std::string& response_id,
    const std::string& model,
    std::string memory_parent_turn_id,
    std::string tool_chain_response_id,
    bool tool_output_pending) {

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
            created_time, json(nullptr), json(nullptr), "", json::object())}
    });

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(state->connection_id);
        if (it != states_.end() && it->second == state) {
            state->recordResponse(
                response_id,
                std::move(messages),
                std::move(memory_parent_turn_id),
                std::move(tool_chain_response_id),
                tool_output_pending);
        }
    }
    state->response_in_flight.store(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// runResponse — full inference execution on a detached thread
// ─────────────────────────────────────────────────────────────────────────────
void WsResponsesController::runResponse(
    drogon::WebSocketConnectionPtr conn,
    std::shared_ptr<WsConnectionState> state,
    CreateChatCompletionRequest sdk_request,
    json mcp_function_tools,
    bool has_mcp_tools,
    std::string response_id,
    std::string model,
    std::string memory_parent_turn_id,
    std::string tool_chain_response_id,
    bool tool_output_submission) {

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
    qai_forge::GenerateOptions options = make_generate_options(
        *state,
        response_id,
        memory_parent_turn_id,
        tool_chain_response_id,
        tool_output_submission);

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
                sdk_request,
                mcp_function_tools,
                ws_emitter,
                response_id,
                options);

            final_response = loop_result.final_response;
            mcp_records    = loop_result.call_records;
            for (const auto& message : loop_result.generated_messages) {
                sdk_request.messages.push_back(message);
            }

        } catch (const GenAIException& e) {
            had_error = true;
            error_msg = e.message;
        } catch (const std::exception& e) {
            had_error = true;
            error_msg = e.what();
        }

    } else {
        // ── Standard path: ModelScheduler::runStreaming() ─────────────────────
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
            auto done_promise = std::make_shared<std::promise<void>>();
            auto done_future = done_promise->get_future();
            auto terminal = std::make_shared<std::atomic<bool>>(false);

            // Use new async API with StreamCallbacks
            qai_forge::StreamCallbacks callbacks;

            callbacks.onToken = [conn, &full_text](const StreamChunk& chunk) {
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
            };

            callbacks.onComplete = [
                &final_response,
                &model,
                done_promise,
                terminal](const StandardResponse& response) {
                final_response = response;
                final_response.model = model;
                if (!terminal->exchange(true)) {
                    done_promise->set_value();
                }
            };

            callbacks.onError = [
                &had_error,
                &error_msg,
                done_promise,
                terminal](const GenAIException& error) {
                had_error = true;
                error_msg = error.message;
                if (!terminal->exchange(true)) {
                    done_promise->set_value();
                }
            };

            callbacks.onCancelled = [
                &had_error,
                &error_msg,
                done_promise,
                terminal]() {
                had_error = true;
                error_msg = "Request was cancelled";
                if (!terminal->exchange(true)) {
                    done_promise->set_value();
                }
            };

            qai_forge::QaiForge::getInstance().generateStream(
                sdk_request,
                std::move(callbacks),
                options);
            done_future.wait();

        } catch (const GenAIException& e) {
            had_error = true;
            error_msg = e.message;
        } catch (const std::exception& e) {
            had_error = true;
            error_msg = e.what();
        }

        if (!had_error) {
            sdk_request.messages.push_back(
                make_assistant_message(final_response));
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
        qai_forge::QaiForge::getInstance().cancel(response_id);
        sendEvent(conn, WsProtocol::make_error(500,
            WsProtocol::ERR_INFERENCE_FAILED, error_msg));

        // Release in-flight flag even on error
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(state->connection_id);
        if (it != states_.end() && it->second == state) {
            state->active_response_id.clear();
            state->response_in_flight.store(false);
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
            created_time, json(nullptr), json(nullptr), "", json::object())}
    });

    // ── Step 5: Update connection-owned transcript and lineage ────────────────
    const bool opens_tool_chain = final_response.tool_calls.has_value() &&
        !final_response.tool_calls->empty();
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = states_.find(state->connection_id);
        if (it != states_.end() && it->second == state) {
            state->recordResponse(
                response_id,
                std::move(sdk_request.messages),
                response_id,
                opens_tool_chain ? response_id : std::string(),
                false);
            state->active_response_id.clear();
            state->response_in_flight.store(false);
        }
    }
}
