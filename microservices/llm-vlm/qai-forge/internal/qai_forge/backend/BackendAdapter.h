// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// BackendAdapter — internal glue between the public qai_forge::
// qai_forge::IGenerativeBackend ABI and the scheduler.
//
// Mirrors OrchestratorAdapter's role for orchestrators — every concrete
// backend (GenIE/LiteRTLM/LlamaCpp, and any future third-party plugin)
// implements the public interface; PluginRegistry wraps the registered
// instance in exactly one of these before handing it to ModelRuntime. Adds
// the placement-aware loadModel() entry point the scheduler actually needs,
// via opportunistic dynamic_cast to IPlacementAwareGenerativeBackend (falls
// back to the public loadModel(model_id) for backends that don't implement
// that mixin).
// ─────────────────────────────────────────────────────────────────────────────
class BackendAdapter {
public:
    explicit BackendAdapter(std::unique_ptr<qai_forge::IGenerativeBackend> impl);

    // Placement-aware load. Falls back to impl_->loadModel(request.model_id)
    // if impl_ doesn't also implement IPlacementAwareGenerativeBackend.
    void loadModel(const qai_forge::execution::RuntimeLaunchRequest& request);

    // Forwarded 1:1 to the wrapped public backend — same signatures as
    // qai_forge::IGenerativeBackend.
    std::string name() const;
    BackendCapabilities capabilities() const;
    void loadModel(const std::string& model_id);
    void unloadModel(bool force = false);
    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_file);
    void generate(
        const std::string& event_id,
        const std::string& session_id,
        const std::string& prompt,
        bool               streaming,
        int                max_tokens,
        float              temperature,
        float              top_p,
        int                top_k,
        float              presence_penalty,
        float              frequency_penalty,
        bool               use_reasoning,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error);
    void generateWithSession(
        const std::string& event_id,
        const std::string& session_id,
        const std::string& prompt,
        bool               streaming,
        int                max_tokens,
        float              temperature,
        float              top_p,
        int                top_k,
        float              presence_penalty,
        float              frequency_penalty,
        bool               use_reasoning,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error,
        bool               kv_invalidated = false);
    void generateVlm(
        const std::string&              event_id,
        const std::string&              session_id,
        const std::string&              prompt,
        const std::vector<std::vector<uint8_t>>& images,
        bool                            streaming,
        int                             max_tokens,
        float                           temperature,
        float                           top_p,
        int                             top_k,
        float                           presence_penalty,
        float                           frequency_penalty,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error);
    void onContextCompacted();
    void resetKvAsync(const std::string& session_id = "");
    void waitForSessionReady(const std::string& session_id = "");
    void saveKv(const std::string& name, const std::string& session_id = "");
    void restoreKv(const std::string& name, const std::string& session_id = "");
    void resetKv(const std::string& session_id = "");
    void clearSession(const std::string& session_id);
    void terminateWorker(bool force = false);
    bool forceKillActiveWorker();
    bool sendAbort(const std::string& session_id);
    bool isHealthy() const;

    // Scheduler/orchestrator code needs the raw public reference to hand to
    // qai_forge::IGenerativeOrchestrator::execute()/postTurn() (which take
    // qai_forge::IGenerativeBackend& per the public ABI, never BackendAdapter).
    qai_forge::IGenerativeBackend& publicBackend() { return *impl_; }

private:
    std::unique_ptr<qai_forge::IGenerativeBackend> impl_;
};
