// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// ResourceTypes - Common physical-resource vocabulary
//
// This header defines the scheduler-visible physical topology primitives:
// ResourceId, ResourceKind, ResourceNode, and PlatformTopology.
//
// These types are provider-agnostic. Provider-native identifiers (QNN
// deviceId, GenIE devices[0].device_id, llama.cpp "HTP1:0", etc.) are never
// stored here directly - they live in ResourceNode::native_ids as opaque
// strings keyed by provider name, and are interpreted only by the
// corresponding execution-provider translation code.
// -----------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace qai_forge {
namespace resources {

// -- ResourceKind -------------------------------------------------------------
// Coarse classification of a physical or shared-domain resource.
enum class ResourceKind {
    Cpu,
    Gpu,
    Htp,               // one physical Hexagon Tensor Processor instance
    Memory,
    AcceleratorDomain  // shared domain such as "cdsp:0" grouping multiple HTPs
};

const char* toString(ResourceKind kind);

// -- ResourceId ----------------------------------------------------------------
// Opaque, human-readable scheduler identity for a physical resource, e.g.
// "htp:0", "htp:1", "cdsp:0", "cpu:0", "system-memory".
struct ResourceId {
    std::string value;

    ResourceId() = default;
    explicit ResourceId(std::string v) : value(std::move(v)) {}

    bool operator==(const ResourceId& other) const { return value == other.value; }
    bool operator!=(const ResourceId& other) const { return !(*this == other); }
    bool operator<(const ResourceId& other) const { return value < other.value; }
};

struct ResourceIdHash {
    size_t operator()(const ResourceId& id) const noexcept {
        return std::hash<std::string>{}(id.value);
    }
};

// -- ResourceNode --------------------------------------------------------------
// One node in the physical topology tree. Parent links express containment,
// e.g. htp:0 and htp:1 are both children of cdsp:0.
struct ResourceNode {
    ResourceId id;
    ResourceKind kind = ResourceKind::Cpu;
    std::optional<ResourceId> parent;
    uint32_t capacity_units = 1;

    // Free-form attributes (e.g. "soc_model", "dsp_arch") for diagnostics.
    std::unordered_map<std::string, std::string> attributes;

    // Provider-native identifier mapping. Key = provider id (e.g. "qnn",
    // "genie", "llamacpp"); value = opaque native selector string
    // (e.g. "1", "HTP1:0"). Absence of a provider key means that provider
    // cannot address this resource directly by native ID (e.g. SNPE, LiteRT).
    std::unordered_map<std::string, std::string> native_ids;
};

// -- PlatformTopology ------------------------------------------------------------
// Immutable snapshot of the platform's physical resource tree, plus metadata
// describing which platform/SKU it was derived from. Constructed by
// TopologyRegistry (see TopologyRegistry.h) - this struct itself performs no
// discovery and holds no I/O state.
struct PlatformTopology {
    std::string platform_name;   // e.g. "IQ9-QCS9075", "VentunoQ", "unknown"
    std::string schema_version = "1.0";
    std::vector<ResourceNode> nodes;

    const ResourceNode* find(const ResourceId& id) const {
        for (const auto& n : nodes) {
            if (n.id == id) return &n;
        }
        return nullptr;
    }

    std::vector<const ResourceNode*> children(const ResourceId& parent) const {
        std::vector<const ResourceNode*> result;
        for (const auto& n : nodes) {
            if (n.parent.has_value() && n.parent.value() == parent) {
                result.push_back(&n);
            }
        }
        return result;
    }

    std::vector<const ResourceNode*> byKind(ResourceKind kind) const {
        std::vector<const ResourceNode*> result;
        for (const auto& n : nodes) {
            if (n.kind == kind) result.push_back(&n);
        }
        return result;
    }
};

}  // namespace resources
}  // namespace qai_forge
