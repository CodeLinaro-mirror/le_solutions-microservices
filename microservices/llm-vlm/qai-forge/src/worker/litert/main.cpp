// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// litert-inference-worker — Layer 3 subprocess for LiteRT Predictive AI
//
// ONE process per runtime type ("litert"), hosting MULTIPLE resident models
// simultaneously — see qnn/main.cpp's header comment for the full design
// rationale (identical pattern: lazy spawn, kept warm indefinitely, one
// dedicated executor thread + independent LiteRTEngine + shm region per
// resident model, shared control-socket write mutex).
//
// Protocol: identical multi-model shape as qnn-inference-worker
// (LOAD_MODEL/UNLOAD_MODEL/EXECUTE, shm fd delivered via SCM_RIGHTS on
// LOAD_MODEL). LOAD_MODEL parameters differ:
//   {"type":"LOAD_MODEL","model_id":"...","model_file":"...",
//    "dispatch_lib_dir":"...","compiler_plugin_dir":"...",
//    "hw_accelerators":<int>,"shm_dir_bytes":<uint>}
//   (hw_accelerators is a flat LOAD_MODEL field, not a nested placement
//    object — it defaults to 5 = NPU|CPU when absent.)
//
// Post-LOAD_MODEL READY always confirms the accelerator mask that was used:
//   {"type":"READY","model_id":"...","provider":"litert",
//    "binding_strength":"automatic","litert":{"hw_accelerators":<int>}}
// ─────────────────────────────────────────────────────────────────────────────

#include "litert-engine.hpp"

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>

using json = nlohmann::ordered_json;

static constexpr size_t kShmAlign = 128;

static size_t alignUp(size_t offset, size_t align) {
    return (offset + align - 1) / align * align;
}

// ─────────────────────────────────────────────────────────────────────────────
// dtype string helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string elementTypeToDtype(int et) {
    switch (et) {
        case 1:  return "FP32";
        case 10: return "FP16";
        case 19: return "BF16";
        case 11: return "FP64";
        case 2:  return "INT32";
        case 4:  return "INT64";
        case 9:  return "INT8";
        case 7:  return "INT16";
        case 3:  return "UINT8";
        case 17: return "UINT16";
        case 16: return "UINT32";
        case 13: return "UINT64";
        case 6:  return "BOOL";
        default: return "FP32";
    }
}

static size_t dtypeBytes(const std::string& dtype) {
    if (dtype == "FP32" || dtype == "INT32" || dtype == "UINT32") return 4;
    if (dtype == "FP16" || dtype == "BF16" || dtype == "INT16" || dtype == "UINT16") return 2;
    if (dtype == "FP64" || dtype == "INT64" || dtype == "UINT64") return 8;
    return 1; // INT8, UINT8, BOOL, etc.
}

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers — line-buffered JSON reader with SCM_RIGHTS fd extraction
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;
static std::mutex g_write_mutex;

struct WorkerExit { int code; };

static void sendMsgLocked(const json& msg) {
    std::string line = msg.dump() + "\n";
    write(g_sock_fd, line.c_str(), line.size());
}

static void sendMsg(const json& msg) {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    sendMsgLocked(msg);
}

static std::string g_read_buf;
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
        if (r <= 0) { std::cerr << "[litert-worker] read timeout/error\n"; throw WorkerExit{1}; }

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
        if (n <= 0) { std::cerr << "[litert-worker] server disconnected\n"; throw WorkerExit{0}; }

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
    std::unique_ptr<LiteRTEngine> engine;
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
            std::vector<const uint8_t*> input_ptrs;
            std::vector<size_t> input_sizes;

            for (const auto& t : inputs_json) {
                const auto& ref = t.at("data_ref");
                size_t offset = ref.value("offset", (size_t)0);
                size_t len = ref.value("len", (size_t)0);
                if (offset + len > slot->shm_dir_bytes)
                    throw std::runtime_error("input data_ref out of bounds");
                input_ptrs.push_back(slot->shm_ptr + offset);
                input_sizes.push_back(len);
            }

            const auto& out_specs = slot->engine->outputSpecs();
            std::vector<uint8_t*> output_ptrs;
            std::vector<size_t> output_sizes;
            std::vector<size_t> output_offsets;
            std::vector<std::string> output_dtypes;

            size_t write_offset = slot->shm_dir_bytes;
            for (size_t i = 0; i < out_specs.size(); ++i) {
                std::string dtype = elementTypeToDtype(out_specs[i].element_type);
                size_t bytes = dtypeBytes(dtype);
                for (auto d : out_specs[i].shape) {
                    int32_t dim = d;
                    if (dim <= 0) dim = 1;
                    bytes *= static_cast<size_t>(dim);
                }

                size_t aligned_offset = alignUp(write_offset, kShmAlign);
                if (aligned_offset + bytes > 2 * slot->shm_dir_bytes)
                    throw std::runtime_error("output tensor '" + out_specs[i].name +
                                              "' exceeds shared-memory capacity");
                std::memset(slot->shm_ptr + aligned_offset, 0, bytes);
                output_ptrs.push_back(slot->shm_ptr + aligned_offset);
                output_sizes.push_back(bytes);
                output_offsets.push_back(aligned_offset);
                output_dtypes.push_back(dtype);
                write_offset = aligned_offset + bytes;
            }

            slot->engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

            json outputs_json = json::array();
            for (size_t i = 0; i < out_specs.size(); ++i) {
                json t;
                t["name"]  = out_specs[i].name;
                t["dtype"] = output_dtypes[i];
                json shape_arr = json::array();
                for (auto d : out_specs[i].shape) shape_arr.push_back(d);
                t["shape"]    = shape_arr;
                t["data_ref"] = {{"offset", output_offsets[i]}, {"len", output_sizes[i]}};
                outputs_json.push_back(t);
            }

            sendMsg({{"type", "RESULT"}, {"event_id", event_id}, {"outputs", outputs_json}});
        } catch (const std::exception& e) {
            sendMsg({{"type", "ERROR"}, {"event_id", event_id}, {"message", e.what()}});
        }
    }
}

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
    if (!fd_env) { std::cerr << "[litert-worker] CONV_SOCKET_FD not set\n"; return 1; }
    g_sock_fd = std::atoi(fd_env);

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

                std::string model_file          = msg.value("model_file",          "");
                std::string dispatch_lib_dir    = msg.value("dispatch_lib_dir",    "/usr/lib");
                std::string compiler_plugin_dir = msg.value("compiler_plugin_dir", "/usr/lib");
                int hw_accelerators             = msg.value("hw_accelerators",     5);

                auto slot = std::make_unique<ModelSlot>();
                slot->model_id = model_id;
                slot->shm_ptr = static_cast<uint8_t*>(mapped);
                slot->shm_dir_bytes = shm_dir_bytes;
                slot->shm_fd = fd;
                slot->engine = std::make_unique<LiteRTEngine>(
                    model_file, dispatch_lib_dir, compiler_plugin_dir, hw_accelerators);

                json ready = {
                    {"type", "READY"},
                    {"model_id", model_id},
                    {"provider", "litert"},
                    {"binding_strength", "automatic"},
                    {"litert", {{"hw_accelerators", hw_accelerators}}}
                };

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

        std::cerr << "[litert-worker] unknown command: " << type << "\n";
    }
    } catch (const WorkerExit& e) {
        teardownAllModels();
        return e.code;
    }

    return 0;
}
