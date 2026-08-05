// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// McpStdioClient.cpp — stdio/subprocess MCP transport implementation
//
// Spawns the MCP server as a child process and communicates via stdin/stdout
// using newline-delimited JSON-RPC 2.0.
//
// Process model:
//   Parent creates two pipes:
//     parent_to_child[write] → child stdin
//     child_to_parent[read]  ← child stdout
//   Child is fork/exec'd with the configured command + args.
//   All communication is synchronous (write request, read response).
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpStdioClient.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cerrno>

// POSIX headers (Linux/Android target)
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
McpStdioClient::McpStdioClient(const McpServerConfig& config)
    : config_(config) {}

McpStdioClient::~McpStdioClient() {
    if (connected_.load()) {
        disconnect();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// connect — spawn subprocess, handshake, cache tools
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_.load()) return;

    spawnProcess();
    doHandshake();

    // Cache the tool list
    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/list";
    req.params = json::object();

    McpResponse resp = sendRequest(req, 10000);
    if (resp.is_error()) {
        killProcess();
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
    std::cout << "[McpStdioClient] Connected to '" << config_.name
              << "' — " << cached_tools_.size() << " tool(s) available\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// disconnect
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) return;
    connected_.store(false);
    killProcess();
    cached_tools_.clear();
}

bool McpStdioClient::isConnected() const {
    return connected_.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// getCachedTools / listTools
// ─────────────────────────────────────────────────────────────────────────────
const std::vector<McpTool>& McpStdioClient::getCachedTools() const {
    return cached_tools_;
}

std::vector<McpTool> McpStdioClient::listTools() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) {
        throw McpException(config_.name, -1, "Not connected");
    }

    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/list";
    req.params = json::object();

    McpResponse resp = sendRequest(req, 10000);
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
McpToolResult McpStdioClient::callTool(const std::string& name, const json& arguments) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_.load()) {
        throw McpException(config_.name, -1, "Not connected");
    }

    McpRequest req;
    req.id     = next_id_++;
    req.method = "tools/call";
    req.params = {{"name", name}, {"arguments", arguments}};

    // Use configured timeout (default 30s)
    const char* timeout_env = std::getenv("RESPONSES_MCP_TOOL_TIMEOUT_MS");
    int timeout_ms = timeout_env ? std::stoi(timeout_env) : 30000;

    McpResponse resp = sendRequest(req, timeout_ms);
    if (resp.is_error()) {
        // Protocol-level error — wrap as an isError tool result
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
// spawnProcess — fork/exec the MCP server subprocess
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::spawnProcess() {
    if (config_.command.empty()) {
        throw McpException(config_.name, -1,
            "stdio transport requires 'command' field in config");
    }

    // Create two pipes: parent→child (stdin) and child→parent (stdout)
    int parent_to_child[2];
    int child_to_parent[2];

    if (pipe(parent_to_child) != 0 || pipe(child_to_parent) != 0) {
        throw McpException(config_.name, -1,
            std::string("pipe() failed: ") + strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(parent_to_child[0]); close(parent_to_child[1]);
        close(child_to_parent[0]); close(child_to_parent[1]);
        throw McpException(config_.name, -1,
            std::string("fork() failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // ── Child process ──────────────────────────────────────────────────
        // Redirect stdin ← parent_to_child[read]
        dup2(parent_to_child[0], STDIN_FILENO);
        // Redirect stdout → child_to_parent[write]
        dup2(child_to_parent[1], STDOUT_FILENO);

        // Close unused ends
        close(parent_to_child[0]); close(parent_to_child[1]);
        close(child_to_parent[0]); close(child_to_parent[1]);

        // Set extra environment variables
        for (const auto& [k, v] : config_.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(config_.command.c_str());
        for (const auto& arg : config_.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(config_.command.c_str(), const_cast<char* const*>(argv.data()));
        // execvp only returns on error
        std::cerr << "[McpStdioClient] execvp failed for '" << config_.command
                  << "': " << strerror(errno) << "\n";
        _exit(1);
    }

    // ── Parent process ─────────────────────────────────────────────────────
    // Close unused ends
    close(parent_to_child[0]);   // child's stdin read end
    close(child_to_parent[1]);   // child's stdout write end

    stdin_write_fd_  = parent_to_child[1];
    stdout_read_fd_  = child_to_parent[0];
    child_pid_       = static_cast<int>(pid);

    // Set stdout read fd to non-blocking for poll()-based reads
    int flags = fcntl(stdout_read_fd_, F_GETFL, 0);
    fcntl(stdout_read_fd_, F_SETFL, flags | O_NONBLOCK);
}

// ─────────────────────────────────────────────────────────────────────────────
// killProcess — terminate and reap the child
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::killProcess() {
    if (stdin_write_fd_ >= 0) { close(stdin_write_fd_); stdin_write_fd_ = -1; }
    if (stdout_read_fd_ >= 0) { close(stdout_read_fd_); stdout_read_fd_ = -1; }

    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        // Give it 1 second to exit gracefully
        int status;
        for (int i = 0; i < 10; ++i) {
            if (waitpid(child_pid_, &status, WNOHANG) > 0) {
                child_pid_ = -1;
                return;
            }
            usleep(100000);  // 100ms
        }
        // Force kill
        kill(child_pid_, SIGKILL);
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// doHandshake — MCP initialize + notifications/initialized
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::doHandshake() {
    // Send initialize request
    McpRequest init_req;
    init_req.id     = next_id_++;
    init_req.method = "initialize";
    init_req.params = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities",    {{"tools", json::object()}}},
        {"clientInfo",      {{"name", "qai-responses"}, {"version", "1.0"}}}
    };

    McpResponse init_resp = sendRequest(init_req, 10000);
    if (init_resp.is_error()) {
        killProcess();
        throw McpException(config_.name, init_resp.error->code,
            "initialize failed: " + init_resp.error->message);
    }

    // Send notifications/initialized (no response expected)
    McpRequest notif = McpRequest::notification("notifications/initialized");
    sendNotification(notif);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeLine — write one JSON line to child stdin
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::writeLine(const std::string& line) {
    std::string data = line + "\n";
    ssize_t written = 0;
    ssize_t total   = static_cast<ssize_t>(data.size());
    while (written < total) {
        ssize_t n = write(stdin_write_fd_, data.c_str() + written,
                          static_cast<size_t>(total - written));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw McpException(config_.name, -1,
                std::string("write() failed: ") + strerror(errno));
        }
        written += n;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// readLine — read one JSON line from child stdout (with timeout)
// ─────────────────────────────────────────────────────────────────────────────
std::string McpStdioClient::readLine(int timeout_ms) {
    std::string line;
    char buf[1];
    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeout_ms);

    while (true) {
        // Check timeout
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw McpException(config_.name, -1,
                "Timeout waiting for response from MCP server '" + config_.name + "'");
        }

        int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        // Poll for data
        struct pollfd pfd;
        pfd.fd     = stdout_read_fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, std::min(remaining_ms, 100));

        if (ret < 0) {
            if (errno == EINTR) continue;
            throw McpException(config_.name, -1,
                std::string("poll() failed: ") + strerror(errno));
        }
        if (ret == 0) continue;  // timeout slice, loop again

        // Data available
        ssize_t n = read(stdout_read_fd_, buf, 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) continue;
            throw McpException(config_.name, -1,
                std::string("read() failed: ") + strerror(errno));
        }
        if (n == 0) {
            // EOF — child process closed stdout
            throw McpException(config_.name, -1,
                "MCP server '" + config_.name + "' closed stdout (process exited?)");
        }

        if (buf[0] == '\n') break;
        line += buf[0];
    }
    return line;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendRequest — write request, read matching response
// ─────────────────────────────────────────────────────────────────────────────
McpResponse McpStdioClient::sendRequest(McpRequest& req, int timeout_ms) {
    std::string request_line = req.to_json().dump();
    writeLine(request_line);

    // Read lines until we get the response matching our request id.
    // (MCP servers may send notifications between responses.)
    while (true) {
        std::string line = readLine(timeout_ms);
        if (line.empty()) continue;

        json j;
        try {
            j = json::parse(line);
        } catch (...) {
            // Skip malformed lines (e.g. server debug output)
            continue;
        }

        // Skip notifications (no "id" field or id is null)
        if (!j.contains("id") || j["id"].is_null()) continue;

        McpResponse resp = McpResponse::from_json(j);
        if (resp.id == req.id) return resp;
        // Different id — skip (shouldn't happen in synchronous protocol)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sendNotification — write notification (no response expected)
// ─────────────────────────────────────────────────────────────────────────────
void McpStdioClient::sendNotification(const McpRequest& notif) {
    writeLine(notif.to_notification_json().dump());
}
