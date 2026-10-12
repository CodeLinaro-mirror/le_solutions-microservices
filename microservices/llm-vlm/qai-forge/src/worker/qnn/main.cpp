// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qnn-inference-worker — Layer 3 subprocess for QNN Predictive AI inference
//
// ONE process per runtime type ("qnn"), hosting MULTIPLE resident models
// simultaneously — not one process per model. Spawned lazily by
// PredictiveWorkerChannel (via PredictiveWorkerProcessPool) on the first QNN
// model load, then kept warm indefinitely and reused for every subsequent
// QNN model. Each loaded model gets its own dedicated executor thread and
// its own independent QNNEngine + shared-memory region — engines are fully
// independent objects, so no cross-model lock is needed around infer(); only
// the shared control socket's write path is protected by a mutex to prevent
// interleaved JSON lines.
//
// If this process crashes (DSP fault, OOM), every QNN model currently loaded
// in it goes unhealthy simultaneously — this is the accepted trade-off of
// pooling models into one process, in exchange for eliminating the
// fork/exec + SDK dlopen + dedicated 64MB shm-region cost that a dedicated
// per-model process previously paid even for tiny classifiers. Recovery is
// automatic: PredictiveWorkerChannel detects the dead socket, respawns this
// binary, and each affected PredictiveModelRuntime naturally reissues
// LOAD_MODEL through its own ensureModelLoaded() retry path.
//
// Protocol (JSON Lines over Unix socket, fd from CONV_SOCKET_FD env var).
// Tensor bytes travel via a per-model shared-memory region; the region's fd
// itself now arrives at runtime — not via fork-time env var inheritance —
// because the worker process already exists by the time a NEW model is
// loaded into it. It is delivered as SCM_RIGHTS ancillary data attached to
// the LOAD_MODEL message's sendmsg() call.
//
//   Startup:
//     Worker → Server: {"type":"READY"}                (process-level, no model_id)
//
//   LOAD_MODEL (load one model into this already-running process):
//     Server → Worker: {"type":"LOAD_MODEL","model_id":"...","model_file":"...",
//                        "backend_lib":"...","sys_lib":"...",
//                        "shm_dir_bytes":<uint>,
//                        "runtime_id":{"model_id":"...","replica_id":<uint>},
//                        "provider":"qnn",
//                        "qnn":{"device_id":<uint>,"require_exact":true}}
//                       (+ shm fd via SCM_RIGHTS ancillary data on the same
//                        sendmsg() call — see recvmsg()/CMSG_* below)
//                       ("qnn" sub-object present only when the resolved
//                        ExecutionPlan requests an exact/physical device;
//                        otherwise the worker binds automatically.)
//     Worker → Server: {"type":"READY","model_id":"...","provider":"qnn",
//                        "runtime_id":{...},
//                        "binding_strength":"exact"|"automatic",
//                        "qnn":{"requested_device_id":<uint>,
//                               "effective_device_id":<uint>}}
//                   or  {"type":"ERROR","model_id":"...","message":"..."}
//
//   UNLOAD_MODEL (free one model's thread+engine+shm without affecting others):
//     Server → Worker: {"type":"UNLOAD_MODEL","model_id":"..."}
//     Worker → Server: {"type":"UNLOADED","model_id":"..."}
//
//   EXECUTE (run inference on an already-loaded model):
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
//     Worker unloads every resident model, joins all executor threads, exits.
// ─────────────────────────────────────────────────────────────────────────────

#include "qnn-engine.hpp"

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>

using json = nlohmann::ordered_json;

// Every tensor's start offset within its shm half is rounded up to this
// boundary — required by SNPE's ExecuteUserBuffers (unaligned -> error 407),
// harmless for QNN. Applied uniformly for consistency with the server-side
// offset packing.
static constexpr size_t kShmAlign = 128;

static size_t alignUp(size_t offset, size_t align) {
    return (offset + align - 1) / align * align;
}

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers — line-buffered JSON reader with SCM_RIGHTS fd extraction
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;
static std::mutex g_write_mutex;

// Thrown instead of calling exit() directly, so all resident model threads
// can be joined and their engines destroyed via normal stack unwinding
// before the process exits (exit() would skip that, leaving driver-owned
// buffers torn down only by the SDK's own atexit hooks, which can race with
// and double-free memory also owned by the engines' destructors).
struct WorkerExit { int code; };

static void sendMsgLocked(const json& msg) {
    std::string line = msg.dump() + "\n";
    write(g_sock_fd, line.c_str(), line.size());
}

static void sendMsg(const json& msg) {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    sendMsgLocked(msg);
}

// Bytes already read from g_sock_fd but not yet consumed as a full line.
static std::string g_read_buf;

// Set by the most recent recvmsg() call that carried SCM_RIGHTS ancillary
// data. Exactly one fd is expected per LOAD_MODEL message and no other
// message type sends one, so a single pending slot (rather than a queue) is
// sufficient given the server serializes writes on its side.
static int g_pending_fd = -1;

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

        struct iovec iov;
        iov.iov_base = chunk;
        iov.iov_len = sizeof(chunk);

        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        std::memset(cmsg_buf, 0, sizeof(cmsg_buf));

        struct msghdr msg_hdr;
        std::memset(&msg_hdr, 0, sizeof(msg_hdr));
        msg_hdr.msg_iov = &iov;
        msg_hdr.msg_iovlen = 1;
        msg_hdr.msg_control = cmsg_buf;
        msg_hdr.msg_controllen = sizeof(cmsg_buf);

        ssize_t n = recvmsg(g_sock_fd, &msg_hdr, 0);
        if (n <= 0) { std::cerr << "[qnn-worker] server disconnected\n"; throw WorkerExit{0}; }

        for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr); cmsg != nullptr;
             cmsg = CMSG_NXTHDR(&msg_hdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int fd = -1;
                std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
                g_pending_fd = fd;
            }
        }

        g_read_buf.append(chunk, static_cast<size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ModelSlot — one resident model's dedicated executor thread + engine + shm
// ─────────────────────────────────────────────────────────────────────────────

struct ModelSlot {
    std::string model_id;
    std::unique_ptr<QNNEngine> engine;
    uint8_t* shm_ptr = nullptr;
    size_t shm_dir_bytes = 0;
    int shm_fd = -1;

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<json> queue;
    bool stop = false;
};

static std::unordered_map<std::string, std::unique_ptr<ModelSlot>> g_models;
static std::mutex g_models_mutex;

static void modelExecutorLoop(ModelSlot* slot) {
    while (true) {
        json msg;
        {
            std::unique_lock<std::mutex> lock(slot->mutex);
            slot->cv.wait(lock, [slot] { return slot->stop || !slot->queue.empty(); });
            if (slot->stop && slot->queue.empty()) break;
            msg = std::move(slot->queue.front());
            slot->queue.pop_front();
        }

        const std::string event_id = msg.value("event_id", "");
        try {
            const auto& inputs_json = msg.at("inputs");
            const auto& in_specs = slot->engine->inputSpecs();
            std::vector<const uint8_t*> input_ptrs(in_specs.size(), nullptr);
            std::vector<size_t> input_sizes(in_specs.size(), 0);

            for (const auto& t : inputs_json) {
                const std::string name = t.at("name").get<std::string>();
                size_t idx = in_specs.size();
                for (size_t i = 0; i < in_specs.size(); ++i) {
                    if (in_specs[i].name == name) { idx = i; break; }
                }
                if (idx == in_specs.size())
                    throw std::runtime_error("EXECUTE: unknown input tensor '" + name + "'");

                const auto& ref = t.at("data_ref");
                size_t offset = ref.value("offset", (size_t)0);
                size_t len = ref.value("len", (size_t)0);
                if (offset + len > slot->shm_dir_bytes)
                    throw std::runtime_error("input data_ref out of bounds");
                input_ptrs[idx] = slot->shm_ptr + offset;
                input_sizes[idx] = len;
            }
            for (size_t i = 0; i < in_specs.size(); ++i) {
                if (!input_ptrs[i])
                    throw std::runtime_error("EXECUTE: missing input tensor '" + in_specs[i].name + "'");
            }

            const auto& out_specs = slot->engine->outputSpecs();
            std::vector<uint8_t*> output_ptrs;
            std::vector<size_t> output_sizes;
            std::vector<size_t> output_offsets;

            size_t write_offset = slot->shm_dir_bytes;
            for (size_t i = 0; i < out_specs.size(); ++i) {
                size_t aligned_offset = alignUp(write_offset, kShmAlign);
                size_t bytes = out_specs[i].bytes;
                if (aligned_offset + bytes > 2 * slot->shm_dir_bytes)
                    throw std::runtime_error("output tensor '" + out_specs[i].name +
                                              "' exceeds shared-memory capacity");
                output_ptrs.push_back(slot->shm_ptr + aligned_offset);
                output_sizes.push_back(bytes);
                output_offsets.push_back(aligned_offset);
                write_offset = aligned_offset + bytes;
            }

            slot->engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

            json outputs_json = json::array();
            for (size_t i = 0; i < out_specs.size(); ++i) {
                json t;
                t["name"] = out_specs[i].name;
                t["dtype"] = out_specs[i].dtype_str;
                json shape_arr = json::array();
                for (auto d : out_specs[i].shape) shape_arr.push_back(d);
                t["shape"] = shape_arr;
                t["data_ref"] = {{"offset", output_offsets[i]}, {"len", output_sizes[i]}};
                outputs_json.push_back(t);
            }

            sendMsg({{"type", "RESULT"}, {"event_id", event_id}, {"outputs", outputs_json}});
        } catch (const std::exception& e) {
            sendMsg({{"type", "ERROR"}, {"event_id", event_id}, {"message", e.what()}});
        }
    }
}

// Stops and joins one model's executor thread, then releases its engine and
// shared-memory mapping. Caller must have already removed `slot` from
// g_models (or hold g_models_mutex across the whole unload).
static void teardownModelSlot(std::unique_ptr<ModelSlot> slot) {
    if (!slot) return;
    {
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->stop = true;
    }
    slot->cv.notify_all();
    if (slot->thread.joinable()) slot->thread.join();
    slot->engine.reset();
    if (slot->shm_ptr) munmap(slot->shm_ptr, 2 * slot->shm_dir_bytes);
    if (slot->shm_fd >= 0) close(slot->shm_fd);
}

static void teardownAllModels() {
    std::unordered_map<std::string, std::unique_ptr<ModelSlot>> models;
    {
        std::lock_guard<std::mutex> lock(g_models_mutex);
        models.swap(g_models);
    }
    for (auto& [id, slot] : models) {
        teardownModelSlot(std::move(slot));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    const char* fd_env = std::getenv("CONV_SOCKET_FD");
    if (!fd_env) { std::cerr << "[qnn-worker] CONV_SOCKET_FD not set\n"; return 1; }
    g_sock_fd = std::atoi(fd_env);

    // Process-level READY — no model_id yet, this just confirms the process
    // itself started and is listening.
    sendMsg({{"type", "READY"}});

    try {
    while (true) {
        json msg = readMsg();
        std::string type = msg.value("type", "");

        if (type == "SHUTDOWN") {
            teardownAllModels();
            break;
        }

        if (type == "LOAD_MODEL") {
            std::string model_id = msg.value("model_id", "");
            int fd = g_pending_fd;
            g_pending_fd = -1;

            try {
                if (fd < 0) {
                    throw std::runtime_error("LOAD_MODEL missing shared-memory fd");
                }

                size_t shm_dir_bytes =
                    msg.value("shm_dir_bytes", static_cast<size_t>(0));
                if (shm_dir_bytes == 0) {
                    throw std::runtime_error("LOAD_MODEL missing shm_dir_bytes");
                }

                void* mapped = mmap(nullptr, 2 * shm_dir_bytes, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, fd, 0);
                if (mapped == MAP_FAILED) {
                    throw std::runtime_error("mmap of shm region failed");
                }

                std::string model_file  = msg.value("model_file",  "");
                std::string backend_lib = msg.value("backend_lib", "/usr/lib/libQnnHtp.so");
                std::string sys_lib     = msg.value("sys_lib",     "/usr/lib/libQnnSystem.so");

                const auto runtime_id = msg.value("runtime_id", json::object());
                if (!runtime_id.is_object() ||
                    runtime_id.value("model_id", "") != model_id ||
                    !runtime_id.contains("replica_id") ||
                    !runtime_id["replica_id"].is_number_unsigned()) {
                    throw std::runtime_error(
                        "LOAD_MODEL runtime_id does not match model_id or is malformed");
                }
                if (msg.value("provider", "") != "qnn") {
                    throw std::runtime_error(
                        "LOAD_MODEL provider is missing or is not qnn");
                }

                std::optional<uint32_t> selected_device_id;
                if (msg.contains("qnn")) {
                    if (!msg["qnn"].is_object() ||
                        !msg["qnn"].contains("device_id") ||
                        !msg["qnn"]["device_id"].is_number_unsigned() ||
                        !msg["qnn"].value("require_exact", false)) {
                        throw std::runtime_error(
                            "invalid exact QNN placement options in LOAD_MODEL");
                    }
                    selected_device_id = msg["qnn"]["device_id"].get<uint32_t>();
                }

                auto slot = std::make_unique<ModelSlot>();
                slot->model_id = model_id;
                slot->shm_ptr = static_cast<uint8_t*>(mapped);
                slot->shm_dir_bytes = shm_dir_bytes;
                slot->shm_fd = fd;
                slot->engine = std::make_unique<QNNEngine>(
                    model_file, backend_lib, sys_lib, selected_device_id);

                json ready = {
                    {"type", "READY"},
                    {"model_id", model_id},
                    {"provider", "qnn"},
                    {"runtime_id", runtime_id}};
                if (selected_device_id.has_value()) {
                    const auto effective = slot->engine->effectiveDeviceId();
                    if (!effective.has_value() ||
                        *effective != *selected_device_id) {
                        throw std::runtime_error(
                            "QNN backend did not confirm requested device_id=" +
                            std::to_string(*selected_device_id));
                    }
                    ready["qnn"] = {
                        {"requested_device_id", *selected_device_id},
                        {"effective_device_id", *effective}
                    };
                    ready["binding_strength"] = "exact";
                } else {
                    ready["binding_strength"] = "automatic";
                }

                ModelSlot* slot_ptr = slot.get();
                slot_ptr->thread = std::thread(modelExecutorLoop, slot_ptr);

                {
                    std::lock_guard<std::mutex> lock(g_models_mutex);
                    g_models[model_id] = std::move(slot);
                }

                sendMsg(ready);
            } catch (const std::exception& e) {
                if (fd >= 0) close(fd);
                sendMsg({{"type", "ERROR"}, {"model_id", model_id}, {"message", e.what()}});
            }
            continue;
        }

        if (type == "UNLOAD_MODEL") {
            std::string model_id = msg.value("model_id", "");
            std::unique_ptr<ModelSlot> slot;
            {
                std::lock_guard<std::mutex> lock(g_models_mutex);
                auto it = g_models.find(model_id);
                if (it != g_models.end()) {
                    slot = std::move(it->second);
                    g_models.erase(it);
                }
            }
            teardownModelSlot(std::move(slot));
            sendMsg({{"type", "UNLOADED"}, {"model_id", model_id}});
            continue;
        }

        if (type == "EXECUTE") {
            std::string model_id = msg.value("model", "");
            std::string event_id = msg.value("event_id", "");

            ModelSlot* slot_ptr = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_models_mutex);
                auto it = g_models.find(model_id);
                if (it != g_models.end()) slot_ptr = it->second.get();
            }

            if (!slot_ptr) {
                sendMsg({{"type", "ERROR"}, {"event_id", event_id},
                         {"message", "Model not loaded: " + model_id}});
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(slot_ptr->mutex);
                slot_ptr->queue.push_back(std::move(msg));
            }
            slot_ptr->cv.notify_one();
            continue;
        }

        std::cerr << "[qnn-worker] unknown command: " << type << "\n";
    }
    } catch (const WorkerExit& e) {
        teardownAllModels();
        return e.code;
    }

    return 0;
}
