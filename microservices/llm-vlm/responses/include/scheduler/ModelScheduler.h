// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "scheduler/CancelResult.h"
#include "scheduler/InferenceJob.h"
#include "scheduler/ToolChainTable.h"
#include "scheduler/WarmModelPool.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scheduler {

struct ModelSchedulerConfig {
    WarmModelPoolConfig pool_config;
    std::chrono::milliseconds tool_response_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds checkpoint_interval = std::chrono::seconds(1);
};

struct SchedulerInvokeOptions {
    std::string response_id;
    std::string previous_response_id;
    std::string session_id;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::NEW_REQUEST;
    bool tool_output_submission = false;
};

// Public scheduler facade.
//
// It owns request-level semantics such as tool-chain continuity and delegates
// model residency/execution ordering to WarmModelPool + ModelRuntime.
class ModelScheduler {
public:
    explicit ModelScheduler(ModelSchedulerConfig config = {},
                            ModelBackendFactory backend_factory = {});
    ~ModelScheduler();

    ModelScheduler(const ModelScheduler&) = delete;
    ModelScheduler& operator=(const ModelScheduler&) = delete;
    ModelScheduler(ModelScheduler&&) = delete;
    ModelScheduler& operator=(ModelScheduler&&) = delete;

    static ModelScheduler& getInstance();

    void start();
    StandardResponse runBlocking(
        const CreateChatCompletionRequest& request,
        const SchedulerInvokeOptions& options = {});
    void runStreaming(
        const CreateChatCompletionRequest& request,
        std::function<void(const StreamChunk&)> callback,
        const SchedulerInvokeOptions& options = {});
    bool cancelResponse(const std::string& response_id);
    SubmitResult submit(InferenceJobPtr job);
    CancelResult cancel(const std::string& job_id);
    void checkpoint();
    ModelPoolSnapshot poolSnapshot() const;
    std::vector<ToolChainEntry> toolChainSnapshot() const;
    void shutdown(bool force = false);
    void stop(bool force = false);

    static int httpStatusForSubmitStatus(SubmitStatus status);

private:
    SubmitResult reject(SubmitStatus status,
                        const std::string& job_id,
                        std::string message) const;
    void prepareToolContinuation(InferenceJob& job,
                                 const ToolChainEntry& chain);
    void wrapCallbacks(InferenceJob& job);
    void handleCompletion(const std::string& response_id,
                          const std::string& model_id,
                          const std::string& session_id,
                          const std::string& previous_chain_id,
                          const StandardResponse& response);
    void closeChainIfPresent(const std::string& model_id,
                             const std::string& chain_id);
    void expireStaleToolChains();
    void tickLoop();

    static bool responseHasToolCalls(const StandardResponse& response);

    ModelSchedulerConfig config_;
    WarmModelPool pool_;
    ToolChainTable tool_chains_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread tick_thread_;
    bool started_ = false;
    bool shutdown_requested_ = false;
};

} // namespace scheduler
