// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// snpe-inference-worker — Layer 3 subprocess for SNPE Predictive AI inference
//
// ONE process per runtime type ("snpe"), hosting MULTIPLE resident models
// simultaneously — see qnn/main.cpp's header comment for the full design
// rationale (this worker follows the identical pattern: lazy spawn, kept
// warm indefinitely, one dedicated executor thread + independent SNPEEngine
// + shm region per resident model, shared control-socket write mutex).
//
// Protocol: identical multi-model shape as qnn-inference-worker
// (LOAD_MODEL/UNLOAD_MODEL/EXECUTE, shm fd delivered via SCM_RIGHTS on
// LOAD_MODEL). INIT parameters differ:
//   {"type":"LOAD_MODEL","model_id":"...","model_file":"...","delegate":"dsp",
//    "output_tensors":["..."],"shm_dir_bytes":<uint>,
//    "snpe":{"runtime_order":"DSP,GPU,CPU"}}
//   ("snpe" is present only when the resolved ExecutionPlan specifies a
//    runtime order; the worker derives "delegate" from it via
//    delegateFromRuntimeOrder(). When absent, the worker's own "delegate"
//    field/default applies — automatic binding.)
//
// Post-LOAD_MODEL READY reports the achieved binding:
//   {"type":"READY","model_id":"...","provider":"snpe",
//    "binding_strength":"preferred"|"automatic",
//    "snpe":{"runtime_order":"...","delegate":"..."}}
//
// SNPE's ExecuteUserBuffers requires 128-byte-aligned I/O buffers (unaligned
// -> error 407 MEMORY_MAPPING_FAILED). Tensor offsets within the shm region
// are always 128-byte aligned by construction (see alignUp() below), so
// pointers taken directly into the mmap'd region satisfy this with no
// separate aligned allocation or copy.
// ─────────────────────────────────────────────────────────────────────────────

#include "snpe-engine.hpp"

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <algorithm>
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

static std::string delegateFromRuntimeOrder(const std::string& order) {
    if (order.empty()) {
        throw std::invalid_argument("SNPE runtime_order must not be empty");
    }

    std::vector<std::string> runtimes;
    size_t start = 0;
    while (start <= order.size()) {
        const size_t comma = order.find(',', start);
        const std::string runtime =
            order.substr(start, comma == std::string::npos
                                   ? std::string::npos
                                   : comma - start);
        if (runtime.empty()) {
            throw std::invalid_argument(
                "SNPE runtime_order contains an empty runtime");
        }
        runtimes.push_back(runtime);
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    for (const auto& runtime : runtimes) {
        if (runtime != "DSP" && runtime != "GPU" &&
            runtime != "AIP_FIXED8_TF" && runtime != "CPU") {
            throw std::invalid_argument(
                "Unsupported SNPE runtime in runtime_order: " + runtime);
        }
    }

    const std::string& first = runtimes.front();
    if (first == "DSP") return "dsp";
    if (first == "GPU") return "gpu";
    if (first == "AIP_FIXED8_TF") return "aip";
    return "none";
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
        if (r <= 0) { std::cerr << "[snpe-worker] read timeout/error\n"; throw WorkerExit{1}; }

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
        if (n <= 0) { std::cerr << "[snpe-worker] server disconnected\n"; throw WorkerExit{0}; }

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
    std::unique_ptr<SNPEEngine> engine;
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

            size_t write_offset = slot->shm_dir_bytes;
            for (size_t i = 0; i < out_specs.size(); ++i) {
                size_t aligned_offset = alignUp(write_offset, kShmAlign);
                size_t bytes = out_specs[i].bytes;
                if (aligned_offset + bytes > 2 * slot->shm_dir_bytes)
                    throw std::runtime_error("output tensor '" + out_specs[i].name +
                                              "' exceeds shared-memory capacity");
                std::memset(slot->shm_ptr + aligned_offset, 0, bytes);
                output_ptrs.push_back(slot->shm_ptr + aligned_offset);
                output_sizes.push_back(bytes);
                output_offsets.push_back(aligned_offset);
                write_offset = aligned_offset + bytes;
            }

            slot->engine->infer(input_ptrs, input_sizes, output_ptrs, output_sizes);

            json outputs_json = json::array();
            for (size_t i = 0; i < out_specs.size(); ++i) {
                json t;
                t["name"]  = out_specs[i].name;
                t["dtype"] = out_specs[i].dtype;
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
    if (!fd_env) { std::cerr << "[snpe-worker] CONV_SOCKET_FD not set\n"; return 1; }
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

                std::string model_file = msg.value("model_file", "");
                std::string delegate = msg.value("delegate", "dsp");
                std::string runtime_order;

                if (msg.contains("snpe") && msg["snpe"].is_object() &&
                    msg["snpe"].contains("runtime_order")) {
                    runtime_order =
                        msg["snpe"]["runtime_order"].get<std::string>();
                    delegate = delegateFromRuntimeOrder(runtime_order);
                }

                std::vector<std::string> output_tensors;
                for (const auto& t :
                     msg.value("output_tensors", json::array())) {
                    output_tensors.push_back(t.get<std::string>());
                }

                auto slot = std::make_unique<ModelSlot>();
                slot->model_id = model_id;
                slot->shm_ptr = static_cast<uint8_t*>(mapped);
                slot->shm_dir_bytes = shm_dir_bytes;
                slot->shm_fd = fd;
                slot->engine = std::make_unique<SNPEEngine>(
                    model_file, delegate, output_tensors);

                json ready = {
                    {"type", "READY"},
                    {"model_id", model_id},
                    {"provider", "snpe"}};
                if (!runtime_order.empty()) {
                    ready["snpe"] = {
                        {"runtime_order", runtime_order},
                        {"delegate", delegate}};
                    ready["binding_strength"] = "preferred";
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

        std::cerr << "[snpe-worker] unknown command: " << type << "\n";
    }
    } catch (const WorkerExit& e) {
        teardownAllModels();
        return e.code;
    }

    return 0;
}
