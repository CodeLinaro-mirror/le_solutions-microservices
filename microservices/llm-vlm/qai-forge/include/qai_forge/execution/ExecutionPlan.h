// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// ExecutionPlan - Provider-neutral placement vocabulary
//
// Describes a concrete placement proposal for one runtime: which provider,
// which components map to which backend targets, and which resource claims
// are required to realize that placement.
// -----------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "qai_forge/resources/ResourceTypes.h"

namespace qai_forge {
namespace execution {

// -- RuntimeId -----------------------------------------------------------------
// Stable identity for one resident model replica. Distinct from model_id:
// multiple RuntimeIds may share the same model_id (replicas).
struct RuntimeId {
    std::string model_id;
    uint64_t replica_id = 0;

    bool operator==(const RuntimeId& other) const {
        return model_id == other.model_id && replica_id == other.replica_id;
    }
    bool operator!=(const RuntimeId& other) const { return !(*this == other); }

    std::string key() const {
        return model_id + "__" + std::to_string(replica_id);
    }
};

struct RuntimeIdHash {
    size_t operator()(const RuntimeId& id) const noexcept {
        size_t h1 = std::hash<std::string>{}(id.model_id);
        size_t h2 = std::hash<uint64_t>{}(id.replica_id);
        return h1 ^ (h2 << 1);
    }
};

// -- TargetSemantics ------------------------------------------------------------
enum class TargetSemantics {
    PhysicalInstance,     // e.g. QNN deviceId=1, GenIE device_id=1
    LogicalEndpoint,      // e.g. llama.cpp "HTP1:0", "HTP0[0-1]"
    RuntimePreference,    // e.g. SNPE runtime_order=AIP_FIXED8_TF,CPU
    AcceleratorClassSet,  // e.g. LiteRT HwAcceleratorSet NPU|CPU
    BackendAutomatic      // e.g. LiteRT-LM backend=npu, no further detail
};

const char* toString(TargetSemantics semantics);

// -- DiscoveryMode --------------------------------------------------------------
enum class DiscoveryMode {
    Enumerated,       // provider can enumerate physical targets directly
    Configured,       // target comes from static configuration (e.g. JSON)
    BackendManaged,   // provider/backend decides internally; not inspectable
    Unavailable       // provider cannot report anything on this platform
};

const char* toString(DiscoveryMode mode);

// -- BindingStrength -------------------------------------------------------------
enum class BindingStrength {
    Exact,       // effective placement is confirmed by the provider/worker
    Preferred,   // requested but not independently confirmed
    Automatic    // provider/backend fully owns placement; no confirmation possible
};

const char* toString(BindingStrength strength);

// -- BackendTarget ---------------------------------------------------------------
// One provider-native selector. target_id is opaque to the scheduler and is
// interpreted only by the provider named in `provider`.
struct BackendTarget {
    std::string provider;    // e.g. "qnn", "genie", "llamacpp", "snpe", "litert"
    std::string target_id;   // opaque, e.g. "device_id=1", "HTP1:0"
    TargetSemantics semantics = TargetSemantics::BackendAutomatic;
    DiscoveryMode discovery_mode = DiscoveryMode::Unavailable;
    BindingStrength binding_strength = BindingStrength::Automatic;
};

// -- ClaimRequirement --------------------------------------------------------------
enum class ClaimRequirement {
    Required,   // must be granted or the plan is rejected
    Fallback,   // used only if the primary/possible claim cannot be granted
    Possible    // may be used; absence does not invalidate the plan
};

const char* toString(ClaimRequirement req);

// -- ResourceClaim ------------------------------------------------------------------
enum class ResourceClaimMode {
    Shared,     // HTP/runtime use may coexist with other shared users
    Exclusive   // conflicts with all other claims on this resource
};

const char* toString(ResourceClaimMode mode);

struct ResourceClaim {
    resources::ResourceId resource;
    uint32_t capacity_units = 1;
    ClaimRequirement requirement = ClaimRequirement::Required;
    // Physical HTP claims are shared by default: Hexagon multiplexes runtime
    // contexts. Use Exclusive only when a provider or hardware test requires
    // sole ownership of this resource/domain.
    ResourceClaimMode mode = ResourceClaimMode::Shared;
};

// -- ComponentRole -------------------------------------------------------------------
enum class ComponentRole {
    MainGraph,
    TextModel,
    VisionEncoder,
    VisionAdapter,
    AudioEncoder,
    Sampler,
    CpuFallback
};

const char* toString(ComponentRole role);

// -- ComponentPlacement -----------------------------------------------------------------
struct ComponentPlacement {
    ComponentRole role = ComponentRole::MainGraph;
    BackendTarget target;
    std::vector<ResourceClaim> claims;
};

// -- ExecutionPlan --------------------------------------------------------------------
// A complete, provider-specific placement proposal for one runtime.
struct ExecutionPlan {
    std::string provider;
    std::vector<ComponentPlacement> components;

    // Free-form native option bag threaded through to materialize(), e.g.
    // {"htp_pd_session": "signed"} - providers may ignore keys they do not
    // recognize.
    std::unordered_map<std::string, std::string> native_options;

    std::vector<ResourceClaim> allClaims() const {
        std::vector<ResourceClaim> result;
        for (const auto& c : components) {
            result.insert(result.end(), c.claims.begin(), c.claims.end());
        }
        return result;
    }
};

// -- ProviderCapabilities -----------------------------------------------------------------
// Static description of what a provider adapter can promise. Used by
// placement policy to decide whether to attempt exact-binding plans.
struct ProviderCapabilities {
    bool enumerates_physical_targets = false;
    bool supports_exact_binding = false;
    bool supports_runtime_preference = false;
    bool supports_fallback = false;
    bool supports_multi_target = false;
    bool reports_effective_target = false;
    bool requires_process_isolation = true;
};

// -- RuntimeLaunchRequest ------------------------------------------------------------------
// Provider-neutral launch input passed by ModelRuntime or
// PredictiveModelRuntime to its owned backend adapter after a plan has been
// chosen and a resource lease acquired.
struct RuntimeLaunchRequest {
    RuntimeId runtime_id;
    std::string model_id;
    ExecutionPlan execution_plan;
};

}  // namespace execution
}  // namespace qai_forge
