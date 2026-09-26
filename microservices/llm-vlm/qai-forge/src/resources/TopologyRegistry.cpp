// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/resources/TopologyRegistry.h"

#include <sstream>
#include <unordered_set>

namespace qai_forge {
namespace resources {

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

}  // namespace

void TopologyRegistry::validate(const PlatformTopology& topology) {
    std::unordered_set<std::string> seen_ids;

    for (const auto& node : topology.nodes) {
        if (node.id.value.empty()) {
            throw InvalidTopologyError(
                "TopologyRegistry: node has empty ResourceId");
        }
        if (!seen_ids.insert(node.id.value).second) {
            throw InvalidTopologyError(
                "TopologyRegistry: duplicate ResourceId '" + node.id.value + "'");
        }
        if (node.parent.has_value() && node.parent.value() == node.id) {
            throw InvalidTopologyError(
                "TopologyRegistry: node '" + node.id.value +
                "' is its own parent");
        }
    }

    for (const auto& node : topology.nodes) {
        if (node.parent.has_value() &&
            seen_ids.find(node.parent.value().value) == seen_ids.end()) {
            throw InvalidTopologyError(
                "TopologyRegistry: node '" + node.id.value +
                "' references unknown parent '" + node.parent.value().value +
                "'");
        }
    }
}

std::unique_ptr<TopologyRegistry> TopologyRegistry::create(
    PlatformTopology topology) {
    validate(topology);
    return std::unique_ptr<TopologyRegistry>(
        new TopologyRegistry(std::move(topology)));
}

std::unique_ptr<TopologyRegistry> TopologyRegistry::createUnknown() {
    PlatformTopology topology;
    topology.platform_name = "unknown";
    topology.schema_version = "1.0";
    return std::unique_ptr<TopologyRegistry>(
        new TopologyRegistry(std::move(topology)));
}

std::string TopologyRegistry::toJson() const {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"schema_version\": \"" << jsonEscape(topology_.schema_version)
        << "\",\n";
    oss << "  \"platform_name\": \"" << jsonEscape(topology_.platform_name)
        << "\",\n";
    oss << "  \"resources\": [\n";

    for (size_t i = 0; i < topology_.nodes.size(); ++i) {
        const auto& node = topology_.nodes[i];
        oss << "    {\n";
        oss << "      \"id\": \"" << jsonEscape(node.id.value) << "\",\n";
        oss << "      \"kind\": \"" << toString(node.kind) << "\",\n";
        if (node.parent.has_value()) {
            oss << "      \"parent\": \"" << jsonEscape(node.parent->value)
                << "\",\n";
        } else {
            oss << "      \"parent\": null,\n";
        }
        oss << "      \"capacity_units\": " << node.capacity_units << ",\n";

        oss << "      \"native_ids\": {";
        {
            bool first = true;
            for (const auto& kv : node.native_ids) {
                if (!first) oss << ",";
                first = false;
                oss << "\n        \"" << jsonEscape(kv.first) << "\": \""
                    << jsonEscape(kv.second) << "\"";
            }
            if (!node.native_ids.empty()) oss << "\n      ";
        }
        oss << "},\n";

        oss << "      \"attributes\": {";
        {
            bool first = true;
            for (const auto& kv : node.attributes) {
                if (!first) oss << ",";
                first = false;
                oss << "\n        \"" << jsonEscape(kv.first) << "\": \""
                    << jsonEscape(kv.second) << "\"";
            }
            if (!node.attributes.empty()) oss << "\n      ";
        }
        oss << "}\n";

        oss << "    }";
        if (i + 1 < topology_.nodes.size()) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}\n";
    return oss.str();
}

}  // namespace resources
}  // namespace qai_forge
