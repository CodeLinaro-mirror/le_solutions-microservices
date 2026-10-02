// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

// ─────────────────────────────────────────────────────────────────────────────
// IPlacementAwareGenerativeBackend — internal mixin interface for
// qai_forge::IGenerativeBackend implementations that support provider-neutral device
// placement.
//
// The public qai_forge::IGenerativeBackend ABI intentionally does NOT
// include this method — placement is an internal, evolving subsystem
// (ExecutionPlan/RuntimeId/ResourceClaim etc., see
// qai_forge/execution/ExecutionPlan.h) not yet promoted to a stable public
// contract. Concrete backends (GenIEBackend/LiteRTLMBackend/LlamaCppBackend)
// implement this as a SECOND base class alongside qai_forge::IGenerativeBackend.
// BackendAdapter dynamic_casts to this interface to opportunistically route
// placement-aware loads; a bare third-party backend that only implements the
// public interface degrades gracefully to loadModel(model_id).
// ─────────────────────────────────────────────────────────────────────────────
class IPlacementAwareGenerativeBackend {
public:
    virtual ~IPlacementAwareGenerativeBackend() = default;
    virtual void loadModel(
        const qai_forge::execution::RuntimeLaunchRequest& request) = 0;
};
