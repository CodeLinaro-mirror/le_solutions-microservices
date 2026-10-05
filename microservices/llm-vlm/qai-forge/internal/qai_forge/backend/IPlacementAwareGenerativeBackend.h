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
//
// DELIBERATE DESIGN BOUNDARY (not an oversight): this mixin lives in
// internal/, not include/, specifically so that no public-ABI-only
// (third-party) IGenerativeBackend implementation can ever participate in
// device placement — only backends compiled as part of qai-forge itself
// (or future first-party plugins given access to this internal header)
// can. This is because the placement subsystem's types (ExecutionPlan,
// RuntimeId, ResourceClaim, PlatformTopology) are still actively evolving
// and are not considered stable enough to freeze into a public contract.
//
// If/when placement is promoted to a public, stable ABI, the correct path
// is a NEW public interface (e.g. qai_forge::IPlacementAwareBackend) that
// third parties can additionally implement — not relaxing this internal
// mixin's visibility. Until that happens, this is a permanent restriction
// of the public backend ABI, not a gap awaiting a trivial fix.
// ─────────────────────────────────────────────────────────────────────────────
class IPlacementAwareGenerativeBackend {
public:
    virtual ~IPlacementAwareGenerativeBackend() = default;
    virtual void loadModel(
        const qai_forge::execution::RuntimeLaunchRequest& request) = 0;
};
