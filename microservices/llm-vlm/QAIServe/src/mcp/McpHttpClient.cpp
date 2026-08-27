// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// McpHttpClient.cpp — HTTP+SSE MCP transport implementation
//
// Connects to a remote MCP server over HTTP. Sends JSON-RPC 2.0 requests as
// HTTP POST to {url}/mcp and reads the JSON response body.
//
// Uses POSIX sockets directly (no external HTTP library dependency).
// For production, replace httpPost() with libcurl or Drogon's HTTP client.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpHttpClient.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <chrono>

// POSIX / socket headers
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
McpHttpClient::McpHttpClient(const McpServerConfig& config)
    : config_(config) {}

McpHttpClient::~McpHttpClient() {
    if (connected_.load()) {
        disconnect();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// connect — parse URL, handshake, cache tools
// ─────────────────────────────────────────────────────────────────────────────
void McpHttpClient::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_.load()) return;

    if (config_.url.empty()) {
        throw McpException(config_.name, -1,
            "http transport requires 'url' field in config");
    }

    parseUrl(config_.url, host_, port_, path_);
    doHandshake();

    // Cache the tool list
    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/list";
    req.params = json::object();

    McpResponse resp = sendHttpRequest(req, 10000);
    if (resp.is_error()) {
        throw McpException(config_.name, resp.error->code,
            "tools/list failed: " + resp.error->message);
    }

    cached_tools_.clear();
    if (resp.result.contains("tools") && resp.result["tools"].is_array()) {
        for (const auto& t : resp.result["tools"]) {
            cached_tools_.push_back(McpTool::from_json(t));
        }
    }

    connected_.store(true);
    std::cout << "[McpHttpClient] Connected to '" << config_.name
              << "' at " << config_.url
              << " — " << cached_tools_.size() << " tool(s) available\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// disconnect
// ─────────────────────────────────────────────────────────────────────────────
void McpHttpClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_.store(false);
    cached_tools_.clear();
}

bool McpHttpClient::isConnected() const {
    return connected_.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// getCachedTools / listTools
// ─────────────────────────────────────────────────────────────────────────────
const std::vector<McpTool>& McpHttpClient::getCachedTools() const {
    return cached_tools_;
}

std::vector<McpTool> McpHttpClient::listTools() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) {
        throw McpException(config_.name, -1, "Not connected");
    }

    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/list";
    req.params = json::object();

    McpResponse resp = sendHttpRequest(req, 10000);
    if (resp.is_error()) {
        throw McpException(config_.name, resp.error->code,
            "tools/list failed: " + resp.error->message);
    }

    cached_tools_.clear();
    if (resp.result.contains("tools") && resp.result["tools"].is_array()) {
        for (const auto& t : resp.result["tools"]) {
            cached_tools_.push_back(McpTool::from_json(t));
        }
    }
    return cached_tools_;
}

// ─────────────────────────────────────────────────────────────────────────────
// callTool
// ─────────────────────────────────────────────────────────────────────────────
McpToolResult McpHttpClient::callTool(const std::string& name, const json& arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) {
        throw McpException(config_.name, -1, "Not connected");
    }

    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/call";
    req.params = {{"name", name}, {"arguments", arguments}};

    const char* timeout_env = std::getenv("RESPONSES_MCP_TOOL_TIMEOUT_MS");
    int timeout_ms = timeout_env ? std::stoi(timeout_env) : 30000;

    McpResponse resp = sendHttpRequest(req, timeout_ms);
    if (resp.is_error()) {
        McpToolResult result;
        result.is_error = true;
        McpContentItem err_item;
        err_item.type = "text";
        err_item.text = "MCP error " + std::to_string(resp.error->code)
                        + ": " + resp.error->message;
        result.content.push_back(err_item);
        return result;
    }

    return McpToolResult::from_json(resp.result);
}

// ─────────────────────────────────────────────────────────────────────────────
// doHandshake
// ─────────────────────────────────────────────────────────────────────────────
void McpHttpClient::doHandshake() {
    McpRequest init_req;
    init_req.id     = next_id_++;
    init_req.method = "initialize";
    init_req.params = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities",    {{"tools", json::object()}}},
        {"clientInfo",      {{"name", "qai-responses"}, {"version", "1.0"}}}
    };

    McpResponse init_resp = sendHttpRequest(init_req, 10000);
    if (init_resp.is_error()) {
        throw McpException(config_.name, init_resp.error->code,
            "initialize failed: " + init_resp.error->message);
    }

    // Send notifications/initialized (fire-and-forget POST, ignore response)
    McpRequest notif = McpRequest::notification("notifications/initialized");
    try {
        httpPost(path_ + "/mcp", notif.to_notification_json().dump(), 5000);
    } catch (...) {
        // Notifications are best-effort
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sendHttpRequest — POST JSON-RPC request, parse response
// ─────────────────────────────────────────────────────────────────────────────
McpResponse McpHttpClient::sendHttpRequest(McpRequest& req, int timeout_ms) {
    std::string body = req.to_json().dump();
    std::string endpoint = path_.empty() ? "/mcp" : path_ + "/mcp";

    std::string response_body = httpPost(endpoint, body, timeout_ms);

    json j;
    try {
        j = json::parse(response_body);
    } catch (const std::exception& e) {
        throw McpException(config_.name, -1,
            std::string("Failed to parse MCP response JSON: ") + e.what()
            + " body: " + response_body.substr(0, 200));
    }

    return McpResponse::from_json(j);
}

// ─────────────────────────────────────────────────────────────────────────────
// httpPost — low-level HTTP POST using POSIX sockets
// ─────────────────────────────────────────────────────────────────────────────
std::string McpHttpClient::httpPost(const std::string& path,
                                     const std::string& body,
                                     int timeout_ms) {
    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port_);

    int rc = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res);
    if (rc != 0) {
        throw McpException(config_.name, -1,
            "getaddrinfo failed for '" + host_ + "': " + gai_strerror(rc));
    }

    // Create socket
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        throw McpException(config_.name, -1,
            std::string("socket() failed: ") + strerror(errno));
    }

    // Set non-blocking for connect with timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Connect
    int conn_rc = ::connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (conn_rc < 0 && errno != EINPROGRESS) {
        close(sock);
        throw McpException(config_.name, -1,
            std::string("connect() failed: ") + strerror(errno));
    }

    if (conn_rc != 0) {
        // Wait for connection
        struct pollfd pfd;
        pfd.fd     = sock;
        pfd.events = POLLOUT;
        int poll_rc = poll(&pfd, 1, timeout_ms);
        if (poll_rc <= 0) {
            close(sock);
            throw McpException(config_.name, -1,
                "Connection timeout to '" + host_ + ":" + port_str + "'");
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            close(sock);
            throw McpException(config_.name, -1,
                std::string("connect() error: ") + strerror(err));
        }
    }

    // Restore blocking mode
    fcntl(sock, F_SETFL, flags);

    // Build HTTP request
    std::ostringstream http_req;
    http_req << "POST " << path << " HTTP/1.1\r\n"
             << "Host: " << host_ << ":" << port_ << "\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Accept: application/json, text/event-stream\r\n"
             << "Connection: close\r\n";
    if (!session_id_.empty()) {
        http_req << "mcp-session-id: " << session_id_ << "\r\n";
    }
    http_req << "\r\n"
             << body;

    std::string req_str = http_req.str();
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(req_str.size())) {
        ssize_t n = send(sock, req_str.c_str() + sent,
                         req_str.size() - static_cast<size_t>(sent), 0);
        if (n < 0) {
            close(sock);
            throw McpException(config_.name, -1,
                std::string("send() failed: ") + strerror(errno));
        }
        sent += n;
    }

    // Read response with timeout
    std::string response;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);
    bool headers_complete = false;
    bool is_sse = false;
    size_t header_end_pos = std::string::npos;
    size_t content_length = 0;
    bool has_content_length = false;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            close(sock);
            throw McpException(config_.name, -1, "HTTP response timeout");
        }
        int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count());

        struct pollfd pfd;
        pfd.fd     = sock;
        pfd.events = POLLIN;
        int poll_rc = poll(&pfd, 1, std::min(remaining, 500));
        if (poll_rc < 0) { break; }
        if (poll_rc == 0) continue;

        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        response += buf;

        // Check if we have complete headers
        if (!headers_complete) {
            header_end_pos = response.find("\r\n\r\n");
            if (header_end_pos != std::string::npos) {
                headers_complete = true;
                // Parse headers
                std::string headers = response.substr(0, header_end_pos);
                is_sse = (headers.find("content-type: text/event-stream") != std::string::npos);

                // Parse Content-Length
                size_t cl_pos = headers.find("content-length: ");
                if (cl_pos != std::string::npos) {
                    size_t cl_start = cl_pos + 16;
                    size_t cl_end = headers.find("\r\n", cl_start);
                    if (cl_end != std::string::npos) {
                        std::string cl_str = headers.substr(cl_start, cl_end - cl_start);
                        content_length = std::stoull(cl_str);
                        has_content_length = true;
                    }
                }

                // Extract mcp-session-id if present (set by initialize response)
                size_t sid_pos = headers.find("mcp-session-id: ");
                if (sid_pos != std::string::npos) {
                    size_t sid_start = sid_pos + 16;
                    size_t sid_end = headers.find("\r\n", sid_start);
                    if (sid_end != std::string::npos) {
                        session_id_ = headers.substr(sid_start, sid_end - sid_start);
                    }
                }
            }
        }

        // Check if we've received complete body based on Content-Length
        if (headers_complete && has_content_length) {
            size_t body_size = response.size() - (header_end_pos + 4);
            if (body_size >= content_length) {
                // Received complete response, stop reading
                break;
            }
        }

        // For SSE: scan body on every iteration (handles multi-packet responses)
        if (headers_complete && is_sse) {
            size_t body_start = header_end_pos + 4;
            std::string body = response.substr(body_start);

            // Look for "data: " line (SSE format: "data: <json>\n")
            size_t data_pos = body.find("data: ");
            if (data_pos != std::string::npos) {
                size_t json_start = data_pos + 6; // after "data: "
                size_t json_end = body.find("\n", json_start);
                if (json_end != std::string::npos) {
                    // Found complete SSE event, extract JSON and close
                    close(sock);
                    return body.substr(json_start, json_end - json_start);
                }
            }
        }
    }
    close(sock);

    // Parse response
    if (header_end_pos == std::string::npos) {
        throw McpException(config_.name, -1,
            "Malformed HTTP response (no header/body separator)");
    }

    std::string body_str = response.substr(header_end_pos + 4);

    // For SSE responses, extract JSON from "data: <json>" line
    if (is_sse) {
        size_t data_pos = body_str.find("data: ");
        if (data_pos != std::string::npos) {
            size_t json_start = data_pos + 6; // after "data: "
            size_t json_end = body_str.find("\n", json_start);
            if (json_end != std::string::npos) {
                return body_str.substr(json_start, json_end - json_start);
            }
        }
    }

    return body_str;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseUrl — extract host, port, path from URL string
// ─────────────────────────────────────────────────────────────────────────────
void McpHttpClient::parseUrl(const std::string& url,
                               std::string& host,
                               int& port,
                               std::string& path) const {
    // Strip scheme
    std::string rest = url;
    bool is_https = false;
    if (rest.substr(0, 8) == "https://") {
        is_https = true;
        rest = rest.substr(8);
    } else if (rest.substr(0, 7) == "http://") {
        rest = rest.substr(7);
    }

    // Split host:port from path
    auto slash_pos = rest.find('/');
    std::string host_port = (slash_pos != std::string::npos)
                            ? rest.substr(0, slash_pos)
                            : rest;
    path = (slash_pos != std::string::npos) ? rest.substr(slash_pos) : "";

    // Split host and port
    auto colon_pos = host_port.find(':');
    if (colon_pos != std::string::npos) {
        host = host_port.substr(0, colon_pos);
        port = std::stoi(host_port.substr(colon_pos + 1));
    } else {
        host = host_port;
        port = is_https ? 443 : 80;
    }
}
