// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// genai-llm-inference-worker — LLM Inference Worker Binary
//
// Subprocess spawned by InferenceWorkerManager for text-generation inference.
// It communicates with the parent process using JSON Lines over a Unix Domain
// Socket and streams inference events back to the parent.
//
// Commands:
//   - INIT: load the requested model
//   - EXECUTE: run blocking or streaming text inference
//   - RESET: clear the per-session KV cache
//   - SHUTDOWN: terminate the worker
//
// The worker supports multiple in-flight EXECUTE requests when the loaded
// model enables bounded continuous batching.
// ─────────────────────────────────────────────────────────────────────────────

#include "llm-engine.hpp"
#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <string>
#include <stdexcept>
#include <random>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>

using json = nlohmann::ordered_json;

static int g_sock_fd = -1;
static std::mutex g_write_mutex;
static std::atomic<bool> g_shutdown{false};

static void send_message(const json& msg) {
    const std::string line = msg.dump() + "\n";
    std::lock_guard<std::mutex> lock(g_write_mutex);
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(g_sock_fd, line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Socket write failed");
        off += static_cast<size_t>(n);
    }
}
static std::string g_read_buf;
static json read_message() {
    char buf[65536];
    for (;;) {
        const size_t pos = g_read_buf.find('\n');
        if (pos != std::string::npos) {
            std::string line = g_read_buf.substr(0, pos);
            g_read_buf.erase(0, pos + 1);
            return json::parse(line);
        }
        const ssize_t n = ::read(g_sock_fd, buf, sizeof(buf));
        if (n <= 0) throw std::runtime_error("Socket closed");
        g_read_buf.append(buf, static_cast<size_t>(n));
    }
}

static std::string resolve_prompt(const json& cmd) {
    return cmd.value("prompt", "");
}

struct Request {
    std::string event_id;
    std::string session_id;
    std::string prompt;
    bool streaming = true;
    GenerationConfig config;
};

struct Lane {
    // Stable for the lifetime of this worker process; identifies the Genie handle lane.
    unsigned long long lane_id = 0;
    std::unique_ptr<LlmEngine> engine;
    std::string model;
    std::string config_file;
    std::string sampler_config;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool busy = false;
    bool executing = false;
    bool failed = false;
    bool reinitializing = false;
    // Set by the sweep thread, under both g_lanes_mutex and mutex, at the
    // instant it selects this idle lane for eviction — closes the race
    // where acquireLane() could otherwise grab the lane in the gap between
    // "found idle" and "signaled to stop".
    bool evicting = false;
    std::string session_id;
    std::unique_ptr<Request> request;
    std::thread thread;
    // Stamped whenever the lane becomes idle (RESET/REINITIALIZE success).
    // Read/written under `mutex`. Used by the sweep thread to evict lanes
    // that have been idle longer than g_idle_timeout.
    std::chrono::steady_clock::time_point idle_since = std::chrono::steady_clock::now();
};

static std::vector<std::unique_ptr<Lane>> g_lanes;
static std::mutex g_lanes_mutex;
static unsigned long long g_next_lane_id = 0;

// Pool-wide config, captured from INIT and reused by the builder thread when
// growing the pool.
static std::string g_model;
static std::string g_config_file;
static std::string g_sampler_config;
static int g_max_slots = 1;
static std::chrono::milliseconds g_idle_timeout{90000};

// ── Lane growth (builder thread) ─────────────────────────────────────────────
// EXECUTE requests that found no idle lane, with room left to grow the pool,
// are parked here instead of failing immediately. A dedicated builder thread
// constructs new lanes one at a time (lanes share one physical engine, so
// builds are serialized rather than run concurrently) off the IPC command
// thread, so RESET/ABORT for unrelated, already-bound sessions are never
// blocked behind a slow GenieDialog_create.
static std::deque<std::unique_ptr<Request>> g_pending_builds;
static std::mutex g_pending_builds_mutex;
static std::condition_variable g_pending_builds_cv;
static std::thread g_builder_thread;
static std::atomic<bool> g_builder_stop{false};

// ── Idle-lane eviction (sweep thread) ────────────────────────────────────────
static std::thread g_sweep_thread;
static std::atomic<bool> g_sweep_stop{false};
static std::mutex g_sweep_cv_mutex;
static std::condition_variable g_sweep_cv;

static void laneLoop(Lane* lane) {
    for (;;) {
        std::unique_ptr<Request> request;
        {
            std::unique_lock<std::mutex> lock(lane->mutex);
            lane->cv.wait(lock, [lane] { return lane->stop || lane->request != nullptr; });
            if (lane->stop && !lane->request) return;
            request = std::move(lane->request);
            lane->executing = true;
        }

        std::string accumulated;
        std::string finish_reason;
        std::string error_message;
        bool success = false;
        try {
            lane->engine->generate(request->prompt, request->config,
                [request = request.get(), &accumulated, &finish_reason](
                    const std::string& token, const std::string& finish) {
                    if (!finish.empty()) {
                        finish_reason = finish;
                        if (request->streaming) {
                            if (!token.empty())
                                send_message({{"type","TOKEN"},{"event_id",request->event_id},
                                              {"session_id",request->session_id},{"content",token}});
                        } else {
                            accumulated += token;
                            send_message({{"type","TOKEN"},{"event_id",request->event_id},
                                          {"session_id",request->session_id},{"content",accumulated}});
                        }
                    } else if (request->streaming) {
                        send_message({{"type","TOKEN"},{"event_id",request->event_id},
                                      {"session_id",request->session_id},{"content",token}});
                    } else {
                        accumulated += token;
                    }
                });
            success = true;
        } catch (const std::exception& e) {
            success = false;
            error_message = e.what();
        }

        // Generation is complete, but the lane remains reserved until the
        // parent sends the asynchronous session RESET command.
        {
            std::lock_guard<std::mutex> lock(lane->mutex);
            lane->executing = false;
        }

        if (success) {
            send_message({{"type","DONE"},{"event_id",request->event_id},
                          {"session_id",request->session_id},
                          {"finish_reason",finish_reason.empty() ? "stop" : finish_reason}});
        } else {
            send_message({{"type","ERROR"},{"event_id",request->event_id},
                          {"session_id",request->session_id},{"message",error_message}});
        }
    }
}

static Lane* acquireLane(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(g_lanes_mutex);
    for (auto& lane : g_lanes) {
        std::lock_guard<std::mutex> lane_lock(lane->mutex);
        if (!lane->busy && !lane->failed && !lane->reinitializing && !lane->request &&
            !lane->evicting) {
            lane->busy = true;
            lane->session_id = session_id;
            LOG_INFO("[llmWorker] LANE_ACQUIRE lane_id=" << lane->lane_id
                     << " session=" << session_id
                     << " lane_count=" << g_lanes.size());
            return lane.get();
        }
    }
    return nullptr;
}

static Lane* findLane(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(g_lanes_mutex);
    for (auto& lane : g_lanes) {
        std::lock_guard<std::mutex> lane_lock(lane->mutex);
        if (lane->busy && lane->session_id == session_id) return lane.get();
    }
    return nullptr;
}

static std::unique_ptr<Request> buildRequestFromCmd(const json& cmd,
                                                      const std::string& event_id,
                                                      const std::string& session_id) {
    auto request = std::make_unique<Request>();
    request->event_id = event_id;
    request->session_id = session_id;
    request->prompt = resolve_prompt(cmd);
    request->streaming = cmd.value("streaming", true);
    request->config.max_tokens = cmd.value("max_tokens", 1024);
    request->config.temperature = cmd.value("temperature", 1.0f);
    request->config.top_p = cmd.value("top_p", 1.0f);
    request->config.top_k = cmd.value("top_k", 40);
    request->config.presence_penalty = cmd.value("presence_penalty", 0.0f);
    request->config.frequency_penalty = cmd.value("frequency_penalty", 0.0f);
    request->config.bypass_think_filter = cmd.value("bypass_think_filter", false);
    return request;
}

static void dispatchToLane(Lane* lane, std::unique_ptr<Request> request) {
    {
        std::lock_guard<std::mutex> lock(lane->mutex);
        lane->request = std::move(request);
    }
    lane->cv.notify_one();
}

// ── Builder thread ────────────────────────────────────────────────────────────
static void builderLoop() {
    for (;;) {
        std::unique_ptr<Request> request;
        {
            std::unique_lock<std::mutex> lock(g_pending_builds_mutex);
            g_pending_builds_cv.wait(lock, [] {
                return g_builder_stop.load() || !g_pending_builds.empty();
            });
            if (g_pending_builds.empty()) {
                if (g_builder_stop.load()) return;
                continue;
            }
            request = std::move(g_pending_builds.front());
            g_pending_builds.pop_front();
        }

        // A lane may have freed up while this request was queued.
        if (Lane* existing = acquireLane(request->session_id)) {
            dispatchToLane(existing, std::move(request));
            continue;
        }

        bool over_capacity = false;
        {
            std::lock_guard<std::mutex> lock(g_lanes_mutex);
            over_capacity = static_cast<int>(g_lanes.size()) >= g_max_slots;
        }
        if (over_capacity) {
            send_message({{"type","ERROR"},{"event_id",request->event_id},
                          {"session_id",request->session_id},
                          {"message","No reusable handle available"}});
            continue;
        }

        try {
            auto lane = std::make_unique<Lane>();
            lane->lane_id = g_next_lane_id++;
            lane->model = g_model;
            lane->config_file = g_config_file;
            lane->sampler_config = g_sampler_config;
            lane->engine = std::make_unique<LlmEngine>(
                lane->model, lane->config_file, lane->sampler_config);
            lane->busy = true;
            lane->session_id = request->session_id;
            Lane* lane_ptr = lane.get();
            lane_ptr->thread = std::thread(laneLoop, lane_ptr);
            {
                std::lock_guard<std::mutex> lock(g_lanes_mutex);
                g_lanes.push_back(std::move(lane));
                LOG_INFO("[llmWorker] LANE_CREATE lane_id=" << lane_ptr->lane_id
                         << " reason=capacity_growth lane_count=" << g_lanes.size());
            }
            dispatchToLane(lane_ptr, std::move(request));
        } catch (const std::exception& e) {
            send_message({{"type","ERROR"},{"event_id",request->event_id},
                          {"session_id",request->session_id},{"message",e.what()}});
        }
    }
}

static void startBuilderThread() {
    g_builder_stop.store(false);
    g_builder_thread = std::thread(builderLoop);
}

static void stopBuilderThread() {
    {
        std::lock_guard<std::mutex> lock(g_pending_builds_mutex);
        g_builder_stop.store(true);
    }
    g_pending_builds_cv.notify_all();
    if (g_builder_thread.joinable()) g_builder_thread.join();

    // Fail any requests that were still parked when the pool was torn down
    // (model switch / shutdown) — nothing will ever build a lane for them now.
    std::deque<std::unique_ptr<Request>> abandoned;
    {
        std::lock_guard<std::mutex> lock(g_pending_builds_mutex);
        abandoned.swap(g_pending_builds);
    }
    for (auto& request : abandoned) {
        try {
            send_message({{"type","ERROR"},{"event_id",request->event_id},
                          {"session_id",request->session_id},
                          {"message","Worker reinitialized before a lane became available"}});
        } catch (...) {}
    }
}

// ── Sweep thread (idle-lane eviction) ────────────────────────────────────────
// Finds and evicts at most one idle lane per call, so callers loop until no
// victim remains. Never shrinks the pool below 1 lane — going to zero lanes
// is the parent's job (unloading the whole worker once the model is idle).
// Finds an idle lane, past its idle timeout, and atomically flags it
// `evicting` before releasing any locks — this closes the race where
// acquireLane() could otherwise grab the lane in between "found idle" and
// "signaled to stop" (see the `evicting` field comment on Lane).
static Lane* findIdleLaneForEviction() {
    std::lock_guard<std::mutex> lock(g_lanes_mutex);
    if (g_lanes.size() <= 1) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    Lane* victim = nullptr;
    auto oldest_idle_since = std::chrono::steady_clock::time_point::max();

    // Select the least-recently-used idle lane, rather than the first lane in
    // vector order, so recently reused lanes remain resident.
    for (auto& lane : g_lanes) {
        std::lock_guard<std::mutex> lane_lock(lane->mutex);
        if (!lane->busy && !lane->executing && !lane->failed && !lane->reinitializing &&
            !lane->evicting && now - lane->idle_since > g_idle_timeout &&
            lane->idle_since < oldest_idle_since) {
            oldest_idle_since = lane->idle_since;
            victim = lane.get();
        }
    }

    if (!victim) return nullptr;

    // g_lanes_mutex prevents acquireLane/findLane from changing this lane
    // while we mark it unavailable.
    {
        std::lock_guard<std::mutex> victim_lock(victim->mutex);
        victim->evicting = true;
    }

    const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - oldest_idle_since).count();
    LOG_INFO("[llmWorker] LANE_IDLE_TIMEOUT lane_id=" << victim->lane_id
             << " idle_ms=" << idle_ms
             << " timeout_ms=" << g_idle_timeout.count());
    return victim;
}

static void evictLane(Lane* victim) {
    const unsigned long long lane_id = victim->lane_id;
    LOG_INFO("[llmWorker] LANE_EVICT_BEGIN lane_id=" << lane_id);
    {
        std::lock_guard<std::mutex> lock(victim->mutex);
        victim->stop = true;
    }
    victim->cv.notify_one();
    if (victim->thread.joinable()) victim->thread.join();

    std::lock_guard<std::mutex> lock(g_lanes_mutex);
    g_lanes.erase(std::remove_if(g_lanes.begin(), g_lanes.end(),
                      [victim](const std::unique_ptr<Lane>& l) { return l.get() == victim; }),
                  g_lanes.end());
    LOG_INFO("[llmWorker] LANE_EVICT_END lane_id=" << lane_id
             << " lane_count=" << g_lanes.size());
}

static void sweepLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_sweep_cv_mutex);
            g_sweep_cv.wait_for(lock, std::chrono::seconds(30),
                                 [] { return g_sweep_stop.load(); });
        }
        if (g_sweep_stop.load()) return;

        for (;;) {
            Lane* victim = findIdleLaneForEviction();
            if (!victim) break;
            evictLane(victim);
        }
    }
}

static void startSweepThread() {
    g_sweep_stop.store(false);
    g_sweep_thread = std::thread(sweepLoop);
}

static void stopSweepThread() {
    {
        std::lock_guard<std::mutex> lock(g_sweep_cv_mutex);
        g_sweep_stop.store(true);
    }
    g_sweep_cv.notify_all();
    if (g_sweep_thread.joinable()) g_sweep_thread.join();
}

int main() {
    const char* socket_fd = std::getenv("LLM_SOCKET_FD");
    if (!socket_fd) return 1;
    g_sock_fd = std::stoi(socket_fd);

    send_message({{"type","READY"}});
    while (!g_shutdown.load()) {
        json cmd;
        try { cmd = read_message(); }
        catch (...) { break; }

        const std::string type = cmd.value("type", "");
        if (type == "INIT") {
            stopBuilderThread();
            stopSweepThread();

            for (auto& lane : g_lanes) {
                { std::lock_guard<std::mutex> lock(lane->mutex); lane->stop = true; }
                lane->cv.notify_one();
            }
            for (auto& lane : g_lanes) if (lane->thread.joinable()) lane->thread.join();
            g_lanes.clear();

            // Only the first lane is built eagerly; the rest grow on demand
            // (see builderLoop) up to g_max_slots as concurrency requires.
            g_max_slots = std::max(1, cmd.value("max_slots", 1));
            g_idle_timeout = std::chrono::milliseconds(
                std::max(1000, cmd.value("idle_timeout_ms", 90000)));
            g_model = cmd.value("model", "");
            g_config_file = cmd.value("config_file", "");
            g_sampler_config = cmd.value("sampler_config", "sampler.json");

            try {
                auto lane = std::make_unique<Lane>();
                lane->lane_id = g_next_lane_id++;
                lane->model = g_model;
                lane->config_file = g_config_file;
                lane->sampler_config = g_sampler_config;
                lane->engine = std::make_unique<LlmEngine>(
                    lane->model, lane->config_file, lane->sampler_config);
                lane->thread = std::thread(laneLoop, lane.get());
                g_lanes.push_back(std::move(lane));

                startBuilderThread();
                LOG_INFO("[llmWorker] LANE_CREATE lane_id=" << g_lanes.back()->lane_id
                         << " reason=initial lane_count=" << g_lanes.size());
                startSweepThread();

                send_message({{"type","READY"}});
            } catch (const std::exception& e) {
                send_message({{"type","ERROR"},{"message",e.what()}});
            }
        } else if (type == "EXECUTE") {
            const std::string event_id = cmd.value("event_id", "");
            const std::string session_id = cmd.value("session_id", "");
            Lane* lane = acquireLane(session_id);
            if (lane) {
                try {
                    dispatchToLane(lane, buildRequestFromCmd(cmd, event_id, session_id));
                } catch (const std::exception& e) {
                    { std::lock_guard<std::mutex> lock(g_lanes_mutex); std::lock_guard<std::mutex> ll(lane->mutex); lane->busy = false; lane->session_id.clear(); }
                    send_message({{"type","ERROR"},{"event_id",event_id},{"session_id",session_id},{"message",e.what()}});
                }
                continue;
            }

            // No idle lane. Grow the pool if under max_slots; the builder
            // thread constructs the new lane off this IPC thread so other
            // sessions' RESET/ABORT are not blocked in the meantime.
            bool can_grow = false;
            {
                std::lock_guard<std::mutex> lock(g_lanes_mutex);
                can_grow = static_cast<int>(g_lanes.size()) < g_max_slots;
            }
            if (!can_grow) {
                send_message({{"type","ERROR"},{"event_id",event_id},
                              {"session_id",session_id},{"message","No reusable handle available"}});
                continue;
            }
            try {
                auto request = buildRequestFromCmd(cmd, event_id, session_id);
                {
                    std::lock_guard<std::mutex> lock(g_pending_builds_mutex);
                    g_pending_builds.push_back(std::move(request));
                }
                g_pending_builds_cv.notify_one();
            } catch (const std::exception& e) {
                send_message({{"type","ERROR"},{"event_id",event_id},{"session_id",session_id},{"message",e.what()}});
            }
        } else if (type == "RESET") {
            const std::string cid = cmd.value("command_id", "");
            const std::string session_id = cmd.value("session_id", "");
            Lane* lane = findLane(session_id);
            if (!lane) {
                send_message({{"type","ERROR"},{"command_id",cid},
                              {"message","No lane found for session"}});
                continue;
            }
            if (lane) {
                std::lock_guard<std::mutex> lock(lane->mutex);
                if (lane->busy && !lane->executing && !lane->request) {
                    if (lane->failed || lane->reinitializing) {
                        send_message({{"type","ERROR"},{"command_id",cid},
                                      {"message","Lane requires reinitialization"}});
                        continue;
                    }
                    try {
                        const unsigned long long lane_id = lane->lane_id;
                        lane->engine->reset();
                        lane->busy = false;
                        lane->failed = false;
                        lane->session_id.clear();
                        lane->idle_since = std::chrono::steady_clock::now();
                        LOG_INFO("[llmWorker] LANE_RELEASE lane_id=" << lane_id
                                 << " reason=kv_reset");
                    } catch (const std::exception& e) {
                        lane->failed = true;
                        send_message({{"type","ERROR"},{"command_id",cid},
                                      {"message",e.what()}});
                        continue;
                    }
                } else if (lane->executing || lane->request) {
                    send_message({{"type","ERROR"},{"command_id",cid},
                                  {"message","Session is still executing"}});
                    continue;
                }
            }
            send_message({{"type","READY"},{"command_id",cid}});
        } else if (type == "REINITIALIZE") {
            const std::string cid = cmd.value("command_id", "");
            const std::string session_id = cmd.value("session_id", "");
            Lane* lane = findLane(session_id);
            if (!lane) {
                send_message({{"type","ERROR"},{"command_id",cid},
                              {"message","No failed lane found for session"}});
                continue;
            }
            std::string model;
            std::string config_file;
            std::string sampler_config;
            {
                std::lock_guard<std::mutex> lock(lane->mutex);
                if (!lane->busy || !lane->failed || lane->executing ||
                    lane->request || lane->reinitializing) {
                    send_message({{"type","ERROR"},{"command_id",cid},
                                  {"message","Lane is not eligible for reinitialization"}});
                    continue;
                }
                lane->reinitializing = true;
                model = lane->model;
                config_file = lane->config_file;
                sampler_config = lane->sampler_config;
            }
            try {
                auto replacement = std::make_unique<LlmEngine>(
                    model, config_file, sampler_config);
                {
                    std::lock_guard<std::mutex> lock(lane->mutex);
                    const unsigned long long lane_id = lane->lane_id;
                    lane->engine = std::move(replacement);
                    lane->failed = false;
                    lane->reinitializing = false;
                    lane->busy = false;
                    lane->session_id.clear();
                    lane->idle_since = std::chrono::steady_clock::now();
                    LOG_INFO("[llmWorker] LANE_RELEASE lane_id=" << lane_id
                             << " reason=reinitialize");
                }
                send_message({{"type","READY"},{"command_id",cid}});
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(lane->mutex);
                lane->reinitializing = false;
                send_message({{"type","ERROR"},{"command_id",cid},
                              {"message",e.what()}});
            }
        } else if (type == "ABORT") {
            const std::string cid = cmd.value("command_id", "");
            Lane* lane = findLane(cmd.value("session_id", ""));
            try { if (lane) lane->engine->abort(); send_message({{"type","READY"},{"command_id",cid}}); }
            catch (const std::exception& e) { send_message({{"type","ERROR"},{"command_id",cid},{"message",e.what()}}); }
        } else if (type == "SAVE_KV" || type == "RESTORE_KV") {
            send_message({{"type","READY"},{"command_id",cmd.value("command_id","")}});
        } else if (type == "SHUTDOWN") {
            g_shutdown.store(true);
            break;
        }
    }

    stopBuilderThread();
    stopSweepThread();

    for (auto& lane : g_lanes) {
        { std::lock_guard<std::mutex> lock(lane->mutex); lane->stop = true; }
        lane->cv.notify_one();
    }
    for (auto& lane : g_lanes) if (lane->thread.joinable()) lane->thread.join();
    g_lanes.clear();
    ::close(g_sock_fd);
    return 0;
}
