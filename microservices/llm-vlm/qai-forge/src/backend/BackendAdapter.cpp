// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/backend/IPlacementAwareGenerativeBackend.h"

#include <utility>

BackendAdapter::BackendAdapter(
    std::unique_ptr<qai_forge::IGenerativeBackend> impl)
    : impl_(std::move(impl)) {}

void BackendAdapter::loadModel(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    if (auto* placement_aware =
            dynamic_cast<IPlacementAwareGenerativeBackend*>(impl_.get())) {
        placement_aware->loadModel(request);
        return;
    }
    impl_->loadModel(request.model_id);
}

std::string BackendAdapter::name() const {
    return impl_->name();
}

BackendCapabilities BackendAdapter::capabilities() const {
    return impl_->capabilities();
}

void BackendAdapter::loadModel(const std::string& model_id) {
    impl_->loadModel(model_id);
}

void BackendAdapter::unloadModel(bool force) {
    impl_->unloadModel(force);
}

void BackendAdapter::ensureWorkerRunning(const std::string& model_id,
                                         const std::string& config_file,
                                         const std::string& sampler_file) {
    impl_->ensureWorkerRunning(model_id, config_file, sampler_file);
}

void BackendAdapter::generate(
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
    std::function<void(const IPCErrorEvent&)>  on_error) {
    impl_->generate(event_id, session_id, prompt, streaming, max_tokens,
                    temperature, top_p, top_k, presence_penalty,
                    frequency_penalty, use_reasoning, std::move(on_token),
                    std::move(on_done), std::move(on_error));
}

void BackendAdapter::generateWithSession(
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
    bool               kv_invalidated) {
    impl_->generateWithSession(event_id, session_id, prompt, streaming,
                               max_tokens, temperature, top_p, top_k,
                               presence_penalty, frequency_penalty,
                               use_reasoning, std::move(on_token),
                               std::move(on_done), std::move(on_error),
                               kv_invalidated);
}

void BackendAdapter::generateVlm(
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
    std::function<void(const IPCErrorEvent&)>  on_error) {
    impl_->generateVlm(event_id, session_id, prompt, images, streaming,
                       max_tokens, temperature, top_p, top_k,
                       presence_penalty, frequency_penalty,
                       std::move(on_token), std::move(on_done),
                       std::move(on_error));
}

void BackendAdapter::onContextCompacted() {
    impl_->onContextCompacted();
}

void BackendAdapter::resetKvAsync(const std::string& session_id) {
    impl_->resetKvAsync(session_id);
}

void BackendAdapter::waitForSessionReady(const std::string& session_id) {
    impl_->waitForSessionReady(session_id);
}

void BackendAdapter::saveKv(const std::string& name,
                            const std::string& session_id) {
    impl_->saveKv(name, session_id);
}

void BackendAdapter::restoreKv(const std::string& name,
                               const std::string& session_id) {
    impl_->restoreKv(name, session_id);
}

void BackendAdapter::resetKv(const std::string& session_id) {
    impl_->resetKv(session_id);
}

void BackendAdapter::clearSession(const std::string& session_id) {
    impl_->clearSession(session_id);
}

void BackendAdapter::terminateWorker(bool force) {
    impl_->terminateWorker(force);
}

bool BackendAdapter::forceKillActiveWorker() {
    return impl_->forceKillActiveWorker();
}

bool BackendAdapter::sendAbort(const std::string& session_id) {
    return impl_->sendAbort(session_id);
}

bool BackendAdapter::isHealthy() const {
    return impl_->isHealthy();
}
