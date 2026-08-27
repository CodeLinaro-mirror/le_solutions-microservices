// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qnn-inference-worker — Layer 3 subprocess for QNN Predictive AI inference
//
// Spawned by QNNBackend (via PredictiveWorkerManager) to provide fault
// isolation. If the QNN backend crashes (DSP fault, OOM), only this process
// dies — the server process is unaffected.
//
// Protocol (JSON Lines over Unix socket, fd from CONV_SOCKET_FD env var):
//
//   Startup:
//     Worker → Server: {"type":"READY"}
//
//   INIT (load model):
//     Server → Worker: {"type":"INIT","model_id":"...","model_file":"...",
//                        "backend_lib":"...","sys_lib":"..."}
//     Worker → Server: {"type":"READY"}  or  {"type":"ERROR","message":"..."}
//
//   EXECUTE (run inference):
//     Server → Worker: {"type":"EXECUTE","event_id":"...","model":"...",
//                        "inputs":[{"name":"...","dtype":"FP32","shape":[...],"data_b64":"..."}],
//                        "output_names":["..."]}
//     Worker → Server: {"type":"RESULT","event_id":"...",
//                        "outputs":[{"name":"...","dtype":"FP32","shape":[...],"data_b64":"..."}]}
//                   or  {"type":"ERROR","event_id":"...","message":"..."}
//
//   SHUTDOWN:
//     Server → Worker: {"type":"SHUTDOWN"}
//     Worker exits cleanly.
// ─────────────────────────────────────────────────────────────────────────────

#include "qnn-engine.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/select.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Base64 (same implementation as PredictiveWorkerManager.cpp)
// ─────────────────────────────────────────────────────────────────────────────

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out += B64_CHARS[(b >> 18) & 0x3F];
        out += B64_CHARS[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[b & 0x3F]        : '=';
    }
    return out;
}

static std::vector<uint8_t> base64Decode(const std::string& s) {
    static const int8_t LUT[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    uint32_t b = 0; int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int8_t v = LUT[(uint8_t)c];
        if (v < 0) continue;
        b = (b << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((b >> bits) & 0xFF)); }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;

// Thrown by readMsg() instead of calling exit() directly. exit() would skip
// destruction of main()'s local `engine` unique_ptr, leaving the QNN backend
// session torn down only by the SDK's own atexit hooks — which double-frees
// driver-owned buffers the engine's destructor also owns. Throwing lets
// main() unwind normally so `engine` is destroyed before the process exits.
struct WorkerExit { int code; };

static void sendMsg(const json& msg) {
    std::string line = msg.dump() + "\n";
    write(g_sock_fd, line.c_str(), line.size());
}

// Bytes already read from g_sock_fd but not yet consumed as a full line —
// carried across readMsg() calls so a single recv() can satisfy multiple/
// partial lines without re-reading one byte at a time (a multi-MB base64
// EXECUTE payload can otherwise cost millions of select()+read() pairs).
static std::string g_read_buf;

static json readMsg() {
    char chunk[65536];
    while (true) {
        size_t newline_pos = g_read_buf.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = g_read_buf.substr(0, newline_pos);
            g_read_buf.erase(0, newline_pos + 1);
            return json::parse(line);
        }

        fd_set fds; FD_ZERO(&fds); FD_SET(g_sock_fd, &fds);
        struct timeval tv{300, 0};
        int r = select(g_sock_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) { std::cerr << "[qnn-worker] read timeout/error\n"; throw WorkerExit{1}; }
        ssize_t n = read(g_sock_fd, chunk, sizeof(chunk));
        if (n <= 0) { std::cerr << "[qnn-worker] server disconnected\n"; throw WorkerExit{0}; }
        g_read_buf.append(chunk, static_cast<size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // Get socket fd from environment
    const char* fd_env = std::getenv("CONV_SOCKET_FD");
    if (!fd_env) { std::cerr << "[qnn-worker] CONV_SOCKET_FD not set\n"; return 1; }
    g_sock_fd = std::atoi(fd_env);

    // Signal ready
    sendMsg({{"type", "READY"}});

    std::unique_ptr<QNNEngine> engine;

    // Command dispatch loop
    // readMsg() throws WorkerExit on timeout/disconnect instead of calling
    // exit() so that `engine` unwinds through its destructor here before
    // the process exits, rather than being torn down implicitly by the SDK's
    // atexit hooks (which previously raced with it and caused a double free).
    try {
    while (true) {
        json msg = readMsg();
        std::string type = msg.value("type", "");

        if (type == "SHUTDOWN") {
            break;
        }

        if (type == "INIT") {
            std::string model_id   = msg.value("model_id",   "");
            std::string model_file = msg.value("model_file", "");
            std::string backend_lib= msg.value("backend_lib","/usr/lib/libQnnHtp.so");
            std::string sys_lib    = msg.value("sys_lib",    "/usr/lib/libQnnSystem.so");

            try {
                engine = std::make_unique<QNNEngine>(model_file, backend_lib, sys_lib);
                sendMsg({{"type", "READY"}});
            } catch (const std::exception& e) {
                sendMsg({{"type", "ERROR"}, {"message", e.what()}});
            }
            continue;
        }

        if (type == "EXECUTE") {
            std::string event_id = msg.value("event_id", "");

            if (!engine) {
                sendMsg({{"type","ERROR"},{"event_id",event_id},{"message","Engine not initialized"}});
                continue;
            }

            try {
                // Decode input tensors
                const auto& inputs_json = msg["inputs"];
                std::vector<std::vector<uint8_t>> input_bufs;
                std::vector<const uint8_t*>       input_ptrs;
                std::vector<size_t>               input_sizes;

                for (const auto& t : inputs_json) {
                    auto buf = base64Decode(t.value("data_b64", ""));
                    input_bufs.push_back(std::move(buf));
                }
                for (auto& buf : input_bufs) {
                    input_ptrs.push_back(buf.data());
                    input_sizes.push_back(buf.size());
                }

                // Allocate output buffers based on engine output specs
                const auto& out_specs = engine->outputSpecs();
                std::vector<std::vector<uint8_t>> output_bufs(out_specs.size());
                std::vector<uint8_t*>             output_ptrs;
                std::vector<size_t>               output_sizes;

                for (size_t i = 0; i < out_specs.size(); ++i) {
                    size_t bytes = 4; // default float32
                    for (auto d : out_specs[i].shape) bytes *= d;
                    output_bufs[i].resize(bytes);
                    output_ptrs.push_back(output_bufs[i].data());
                    output_sizes.push_back(bytes);
                }

                // Run inference
                engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

                // Build RESULT response
                json outputs_json = json::array();
                for (size_t i = 0; i < out_specs.size(); ++i) {
                    json t;
                    t["name"]     = out_specs[i].name;
                    t["dtype"]    = "FP32";
                    json shape_arr = json::array();
                    for (auto d : out_specs[i].shape) shape_arr.push_back(d);
                    t["shape"]    = shape_arr;
                    t["data_b64"] = base64Encode(output_bufs[i].data(), output_bufs[i].size());
                    outputs_json.push_back(t);
                }

                sendMsg({{"type","RESULT"},{"event_id",event_id},{"outputs",outputs_json}});

            } catch (const std::exception& e) {
                sendMsg({{"type","ERROR"},{"event_id",event_id},{"message",e.what()}});
            }
            continue;
        }

        // Unknown command
        std::cerr << "[qnn-worker] unknown command: " << type << "\n";
    }
    } catch (const WorkerExit& e) {
        return e.code;
    }

    return 0;
}
