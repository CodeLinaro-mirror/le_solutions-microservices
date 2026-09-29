// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// TopologyRegistry - Owns and validates the platform's physical topology
//
// TopologyRegistry is a pure data holder: it validates a PlatformTopology at
// construction time (unique IDs, valid parent references) and exposes
// read-only lookup. It performs no I/O and no hardware discovery - discovery
// is the job of platform-specific probes (e.g. QnnTopologyProbe) which
// produce a PlatformTopology that is then handed to this registry.
//
// Thread-safety: after construction, TopologyRegistry is immutable and safe
// to share across threads via const reference or shared_ptr<const
// TopologyRegistry>.
// -----------------------------------------------------------------------------

#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "qai_forge/resources/ResourceTypes.h"

namespace qai_forge {
namespace resources {

// Thrown by TopologyRegistry::create() when the supplied topology is
// structurally invalid (duplicate IDs, dangling parent references, etc).
class InvalidTopologyError : public std::runtime_error {
public:
    explicit InvalidTopologyError(const std::string& msg)
        : std::runtime_error(msg) {}
};

class TopologyRegistry {
public:
    // Validates `topology` and returns an owning registry. Throws
    // InvalidTopologyError on structural problems:
    //   - duplicate ResourceId values
    //   - a node whose `parent` does not reference an existing node
    //   - a node that is its own parent
    static std::unique_ptr<TopologyRegistry> create(PlatformTopology topology);

    // Convenience factory for platforms with no known topology (e.g. an
    // unrecognized SKU, or a build with hardware discovery disabled).
    // Produces a registry with zero nodes and platform_name "unknown".
    static std::unique_ptr<TopologyRegistry> createUnknown();

    const PlatformTopology& topology() const { return topology_; }

    const ResourceNode* find(const ResourceId& id) const {
        return topology_.find(id);
    }

    std::vector<const ResourceNode*> children(const ResourceId& parent) const {
        return topology_.children(parent);
    }

    std::vector<const ResourceNode*> byKind(ResourceKind kind) const {
        return topology_.byKind(kind);
    }

    // Serializes the topology to a deterministic JSON string.
    // Nodes are emitted in the order they appear in topology().nodes -
    // callers that need deterministic output across runs should ensure
    // that order is deterministic when they build the PlatformTopology
    // (e.g. sort by ResourceId before calling create()).
    std::string toJson() const;

private:
    explicit TopologyRegistry(PlatformTopology topology)
        : topology_(std::move(topology)) {}

    static void validate(const PlatformTopology& topology);

    PlatformTopology topology_;
};

}  // namespace resources
}  // namespace qai_forge
