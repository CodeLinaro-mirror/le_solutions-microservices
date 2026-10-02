// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/PredictiveBackendAdapter.h"
#include "qai_forge/backend/IPlacementAwarePredictiveBackend.h"

#include <utility>

PredictiveBackendAdapter::PredictiveBackendAdapter(
    std::unique_ptr<qai_forge::IInferenceBackend> impl)
    : impl_(std::move(impl)) {}

void PredictiveBackendAdapter::initialize(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    if (auto* placement_aware =
            dynamic_cast<IPlacementAwarePredictiveBackend*>(impl_.get())) {
        placement_aware->initialize(request);
        return;
    }
    impl_->initialize(request.model_id, {});
}

std::string PredictiveBackendAdapter::name() const {
    return impl_->name();
}

void PredictiveBackendAdapter::initialize(const std::string& model_id,
                                          const std::string& model_file) {
    impl_->initialize(model_id, model_file);
}

TensorInferenceResponse PredictiveBackendAdapter::infer(
    const SegmentedTensorInferenceRequest& request) {
    return impl_->infer(request);
}

PredictiveExecuteRequest PredictiveBackendAdapter::prepareWorkerRequest(
    const SegmentedTensorInferenceRequest& request) {
    return impl_->prepareWorkerRequest(request);
}

bool PredictiveBackendAdapter::isHealthy() const {
    return impl_->isHealthy();
}

void PredictiveBackendAdapter::shutdown() {
    impl_->shutdown();
}
