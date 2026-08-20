// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qnn-inference-worker — Layer 3 subprocess for QNN Predictive AI inference
//
// Spawned by QNNBackend (via PredictiveWorkerManager) to provide fault
// isolation. If the QNN backend crashes (DSP fault, OOM), only this process
// dies — the server process is unaffected.
//
// Protocol (JSON Lines over Unix socket, fd from CONV_SOCKET_FD env var).
// Tensor bytes travel via a shared-memory region (memfd inherited from the
// parent via CONV_SHM_FD, sized CONV_SHM_DIR_BYTES per direction) rather than
// inline in JSON — messages carry only a data_ref {offset,len} into it:
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
//                        "inputs":[{"name":"...","dtype":"FP32","shape":[...],
//                                   "data_ref":{"offset":0,"len":602112}}],
//                        "output_names":["..."]}
//     Worker → Server: {"type":"RESULT","event_id":"...",
//                        "outputs":[{"name":"...","dtype":"FP32","shape":[...],
//                                    "data_ref":{"offset":33554432,"len":4000}}]}
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
#include <sys/mman.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Shared-memory tensor region (see PredictiveWorkerManager for full design)
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t* g_shm_ptr       = nullptr;
static size_t   g_shm_dir_bytes = 0;

// Every tensor's start offset within its shm half is rounded up to this
// boundary. Only strictly required by SNPE's ExecuteUserBuffers, but applied
// here too for consistency with the server-side offset packing.
static constexpr size_t kShmAlign = 128;

static size_t alignUp(size_t offset, size_t align) {
    return (offset + align - 1) / align * align;
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

    // Attach the shared-memory tensor region inherited from the parent.
    const char* shm_fd_env = std::getenv("CONV_SHM_FD");
    const char* shm_bytes_env = std::getenv("CONV_SHM_DIR_BYTES");
    if (!shm_fd_env || !shm_bytes_env) {
        std::cerr << "[qnn-worker] CONV_SHM_FD/CONV_SHM_DIR_BYTES not set\n";
        return 1;
    }
    int shm_fd = std::atoi(shm_fd_env);
    g_shm_dir_bytes = std::strtoull(shm_bytes_env, nullptr, 10);
    void* mapped = mmap(nullptr, 2 * g_shm_dir_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED, shm_fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "[qnn-worker] mmap of shm region failed\n";
        return 1;
    }
    g_shm_ptr = static_cast<uint8_t*>(mapped);

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
                // Inputs: point directly into the shm input half via data_ref
                // — no decode buffer.
                const auto& inputs_json = msg["inputs"];
                std::vector<const uint8_t*> input_ptrs;
                std::vector<size_t>         input_sizes;

                for (const auto& t : inputs_json) {
                    const auto& ref = t.at("data_ref");
                    size_t offset = ref.value("offset", (size_t)0);
                    size_t len    = ref.value("len", (size_t)0);
                    if (offset + len > g_shm_dir_bytes)
                        throw std::runtime_error("input data_ref out of bounds");
                    input_ptrs.push_back(g_shm_ptr + offset);
                    input_sizes.push_back(len);
                }

                // Outputs: lay out each tensor at a 128-byte-aligned offset
                // within the shm output half [g_shm_dir_bytes,
                // 2*g_shm_dir_bytes), and hand engine->infer() pointers
                // directly into shared memory — it writes results straight
                // there, no intermediate buffer.
                const auto& out_specs = engine->outputSpecs();
                std::vector<uint8_t*> output_ptrs;
                std::vector<size_t>   output_sizes;
                std::vector<size_t>   output_offsets;

                size_t write_offset = g_shm_dir_bytes;
                for (size_t i = 0; i < out_specs.size(); ++i) {
                    size_t aligned_offset = alignUp(write_offset, kShmAlign);
                    size_t bytes = out_specs[i].bytes;
                    if (aligned_offset + bytes > 2 * g_shm_dir_bytes)
                        throw std::runtime_error("output tensor '" + out_specs[i].name +
                                                  "' exceeds shared-memory capacity");
                    output_ptrs.push_back(g_shm_ptr + aligned_offset);
                    output_sizes.push_back(bytes);
                    output_offsets.push_back(aligned_offset);
                    write_offset = aligned_offset + bytes;
                }

                // Run inference — writes outputs directly into shared memory
                engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

                // Build RESULT response
                json outputs_json = json::array();
                for (size_t i = 0; i < out_specs.size(); ++i) {
                    json t;
                    t["name"]     = out_specs[i].name;
                    t["dtype"]    = out_specs[i].dtype_str;
                    json shape_arr = json::array();
                    for (auto d : out_specs[i].shape) shape_arr.push_back(d);
                    t["shape"]    = shape_arr;
                    t["data_ref"] = {{"offset", output_offsets[i]}, {"len", output_sizes[i]}};
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
