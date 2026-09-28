// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/worker/PredictiveWorkerProcessPool.h"

PredictiveWorkerProcessPool& PredictiveWorkerProcessPool::getInstance() {
    static PredictiveWorkerProcessPool instance;
    return instance;
}

PredictiveWorkerChannel& PredictiveWorkerProcessPool::getChannel(
    const std::string& worker_binary,
    const std::string& process_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(worker_binary);
    if (it == channels_.end()) {
        auto channel =
            std::make_unique<PredictiveWorkerChannel>(worker_binary, process_type);
        auto [inserted_it, ok] =
            channels_.emplace(worker_binary, std::move(channel));
        it = inserted_it;
    }
    return *it->second;
}

void PredictiveWorkerProcessPool::shutdownAll() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [binary, channel] : channels_) {
        if (channel) {
            channel->terminate();
        }
    }
    channels_.clear();
}
