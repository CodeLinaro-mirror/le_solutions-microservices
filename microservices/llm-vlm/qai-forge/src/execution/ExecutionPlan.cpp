// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/ExecutionPlan.h"

namespace qai_forge {
namespace execution {

const char* toString(TargetSemantics semantics) {
    switch (semantics) {
        case TargetSemantics::PhysicalInstance:    return "physical_instance";
        case TargetSemantics::LogicalEndpoint:     return "logical_endpoint";
        case TargetSemantics::RuntimePreference:   return "runtime_preference";
        case TargetSemantics::AcceleratorClassSet: return "accelerator_class_set";
        case TargetSemantics::BackendAutomatic:    return "backend_automatic";
        default:                                   return "unknown";
    }
}

const char* toString(DiscoveryMode mode) {
    switch (mode) {
        case DiscoveryMode::Enumerated:     return "enumerated";
        case DiscoveryMode::Configured:     return "configured";
        case DiscoveryMode::BackendManaged: return "backend_managed";
        case DiscoveryMode::Unavailable:    return "unavailable";
        default:                            return "unknown";
    }
}

const char* toString(BindingStrength strength) {
    switch (strength) {
        case BindingStrength::Exact:     return "exact";
        case BindingStrength::Preferred: return "preferred";
        case BindingStrength::Automatic: return "automatic";
        default:                         return "unknown";
    }
}

const char* toString(ClaimRequirement req) {
    switch (req) {
        case ClaimRequirement::Required: return "required";
        case ClaimRequirement::Fallback: return "fallback";
        case ClaimRequirement::Possible: return "possible";
        default:                         return "unknown";
    }
}

const char* toString(ResourceClaimMode mode) {
    switch (mode) {
        case ResourceClaimMode::Exclusive: return "exclusive";
        case ResourceClaimMode::Shared:    return "shared";
        default:                           return "unknown";
    }
}

const char* toString(ComponentRole role) {
    switch (role) {
        case ComponentRole::MainGraph:     return "main_graph";
        case ComponentRole::TextModel:     return "text_model";
        case ComponentRole::VisionEncoder: return "vision_encoder";
        case ComponentRole::VisionAdapter: return "vision_adapter";
        case ComponentRole::AudioEncoder:  return "audio_encoder";
        case ComponentRole::Sampler:       return "sampler";
        case ComponentRole::CpuFallback:   return "cpu_fallback";
        default:                           return "unknown";
    }
}

}  // namespace execution
}  // namespace qai_forge
