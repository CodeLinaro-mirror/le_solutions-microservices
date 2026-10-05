// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

// ─────────────────────────────────────────────────────────────────────────────
// IPlacementAwarePredictiveBackend — internal mixin interface for
// qai_forge::IInferenceBackend implementations that support provider-neutral device
// placement.
//
// Mirrors IPlacementAwareGenerativeBackend's role on the generative side.
// The public qai_forge::IInferenceBackend ABI intentionally does NOT include
// this method — placement is an internal, evolving subsystem not yet
// promoted to a stable public contract. Concrete backends (QNNBackend/
// SNPEBackend/LiteRTBackend) implement this as a SECOND base class alongside
// qai_forge::IInferenceBackend. PredictiveBackendAdapter dynamic_casts to
// this interface to opportunistically route placement-aware initialization;
// a bare third-party backend that only implements the public interface
// degrades gracefully to initialize(model_id, {}).
//
// DELIBERATE DESIGN BOUNDARY (not an oversight) — see
// IPlacementAwareGenerativeBackend.h's matching note for the full
// rationale. This mixin's internal-only visibility is intentional and
// permanent until/unless the placement subsystem (ExecutionPlan/RuntimeId/
// ResourceClaim) is deliberately promoted to a stable public ABI via a new
// public interface — not by relaxing this header's visibility.
// ─────────────────────────────────────────────────────────────────────────────
class IPlacementAwarePredictiveBackend {
public:
    virtual ~IPlacementAwarePredictiveBackend() = default;
    virtual void initialize(
        const qai_forge::execution::RuntimeLaunchRequest& request) = 0;
};
