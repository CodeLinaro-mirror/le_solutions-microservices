// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// snpe-inference-worker — Layer 3 subprocess for SNPE Predictive AI inference
//
// Spawned by SNPEBackend (via PredictiveWorkerManager) to provide fault
// isolation. If the SNPE backend crashes (DSP fault, OOM), only this process
// dies — the server process is unaffected.
//
// Protocol: identical to qnn-inference-worker (JSON Lines over Unix socket).
// INIT command parameters differ:
//   {"type":"INIT","model_id":"...","model_file":"...","delegate":"dsp",
//    "output_tensors":["..."]}
// ─────────────────────────────────────────────────────────────────────────────

#include "snpe-engine.hpp"

#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/select.h>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
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

static void sendMsg(const json& msg) {
    std::string line = msg.dump() + "\n";
    write(g_sock_fd, line.c_str(), line.size());
}

static json readMsg() {
    std::string line;
    char ch;
    while (true) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g_sock_fd, &fds);
        struct timeval tv{300, 0};
        int r = select(g_sock_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) { std::cerr << "[snpe-worker] read timeout/error\n"; exit(1); }
        ssize_t n = read(g_sock_fd, &ch, 1);
        if (n <= 0) { std::cerr << "[snpe-worker] server disconnected\n"; exit(0); }
        if (ch == '\n') break;
        line += ch;
    }
    return json::parse(line);
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    const char* fd_env = std::getenv("CONV_SOCKET_FD");
    if (!fd_env) { std::cerr << "[snpe-worker] CONV_SOCKET_FD not set\n"; return 1; }
    g_sock_fd = std::atoi(fd_env);

    sendMsg({{"type", "READY"}});

    std::unique_ptr<SNPEEngine> engine;

    while (true) {
        json msg = readMsg();
        std::string type = msg.value("type", "");

        if (type == "SHUTDOWN") break;

        if (type == "INIT") {
            std::string model_file = msg.value("model_file", "");
            std::string delegate   = msg.value("delegate",   "dsp");

            std::vector<std::string> output_tensors;
            for (const auto& t : msg.value("output_tensors", json::array()))
                output_tensors.push_back(t.get<std::string>());

            try {
                engine = std::make_unique<SNPEEngine>(model_file, delegate, output_tensors);
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
                // Decode input tensors into 128-byte aligned buffers.
                // SNPE DSP (ExecuteUserBuffers) requires 128-byte alignment;
                // unaligned addresses produce error 407 (MEMORY_MAPPING_FAILED).
                const auto& inputs_json = msg["inputs"];
                struct AlignedBuf {
                    void*  ptr  = nullptr;
                    size_t size = 0;
                    ~AlignedBuf() { if (ptr) free(ptr); }
                };
                std::vector<AlignedBuf>       input_bufs(inputs_json.size());
                std::vector<const uint8_t*>   input_ptrs;
                std::vector<size_t>           input_sizes;

                for (size_t i = 0; i < inputs_json.size(); ++i) {
                    auto decoded = base64Decode(inputs_json[i].value("data_b64", ""));
                    size_t sz = decoded.size();
                    void* aligned = nullptr;
                    if (posix_memalign(&aligned, 128, sz ? sz : 1) != 0)
                        throw std::runtime_error("posix_memalign failed for input " + std::to_string(i));
                    memcpy(aligned, decoded.data(), sz);
                    input_bufs[i].ptr  = aligned;
                    input_bufs[i].size = sz;
                }
                for (auto& buf : input_bufs) {
                    input_ptrs.push_back(static_cast<const uint8_t*>(buf.ptr));
                    input_sizes.push_back(buf.size);
                }

                // Allocate output buffers — 128-byte aligned, same requirement as inputs.
                const auto& out_specs = engine->outputSpecs();
                std::vector<AlignedBuf> output_bufs(out_specs.size());
                std::vector<uint8_t*>   output_ptrs;
                std::vector<size_t>     output_sizes;

                for (size_t i = 0; i < out_specs.size(); ++i) {
                    size_t sz = out_specs[i].bytes;
                    void* aligned = nullptr;
                    if (posix_memalign(&aligned, 128, sz ? sz : 1) != 0)
                        throw std::runtime_error("posix_memalign failed for output " + std::to_string(i));
                    memset(aligned, 0, sz);
                    output_bufs[i].ptr  = aligned;
                    output_bufs[i].size = sz;
                    output_ptrs.push_back(static_cast<uint8_t*>(aligned));
                    output_sizes.push_back(sz);
                }

                // Run inference
                engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

                // Build RESULT response
                json outputs_json = json::array();
                for (size_t i = 0; i < out_specs.size(); ++i) {
                    json t;
                    t["name"]  = out_specs[i].name;
                    t["dtype"] = out_specs[i].dtype;
                    json shape_arr = json::array();
                    for (auto d : out_specs[i].shape) shape_arr.push_back(d);
                    t["shape"]    = shape_arr;
                    t["data_b64"] = base64Encode(static_cast<const uint8_t*>(output_bufs[i].ptr), output_bufs[i].size);
                    outputs_json.push_back(t);
                }

                sendMsg({{"type","RESULT"},{"event_id",event_id},{"outputs",outputs_json}});

            } catch (const std::exception& e) {
                sendMsg({{"type","ERROR"},{"event_id",event_id},{"message",e.what()}});
            }
            continue;
        }

        std::cerr << "[snpe-worker] unknown command: " << type << "\n";
    }

    return 0;
}
