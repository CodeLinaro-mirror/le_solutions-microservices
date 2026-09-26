// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/LlamaCppLaunchOptions.h"

#include <cctype>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace qai_forge {
namespace execution {

namespace {

const std::regex& endpointPattern() {
    static const std::regex pattern(
        R"(HTP[0-9]+(?::[0-9]+|\[[0-9]+-[0-9]+\])?)",
        std::regex::ECMAScript);
    return pattern;
}

void validateEndpointItem(const std::string& item) {
    if (item.empty() ||
        !std::regex_match(item, endpointPattern())) {
        throw std::invalid_argument(
            "LlamaCppLaunchOptions: malformed Hexagon endpoint '" + item +
            "'");
    }

    const auto open = item.find('[');
    if (open != std::string::npos) {
        const auto dash = item.find('-', open);
        const auto close = item.find(']', dash);
        const auto first = std::stoull(item.substr(open + 1, dash - open - 1));
        const auto last = std::stoull(item.substr(dash + 1, close - dash - 1));
        if (first > last) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: descending endpoint group '" + item +
                "'");
        }
    }
}

void validateEndpointSelector(const std::string& selector) {
    if (selector.empty()) {
        throw std::invalid_argument(
            "LlamaCppLaunchOptions: explicit endpoint selector is empty");
    }

    std::unordered_set<std::string> seen;
    size_t start = 0;
    while (start <= selector.size()) {
        const size_t comma = selector.find(',', start);
        const std::string item = selector.substr(
            start, comma == std::string::npos ? std::string::npos
                                               : comma - start);

        for (const unsigned char character : item) {
            if (std::isspace(character)) {
                throw std::invalid_argument(
                    "LlamaCppLaunchOptions: endpoint selector must not contain "
                    "whitespace");
            }
        }

        validateEndpointItem(item);
        if (!seen.insert(item).second) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: duplicate endpoint '" + item + "'");
        }

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
}

std::optional<std::string> selectedEndpoint(const ExecutionPlan& plan) {
    if (plan.provider != "llamacpp") {
        throw std::invalid_argument(
            "LlamaCppLaunchOptions: expected a llama.cpp execution plan");
    }

    std::optional<std::string> selected;
    for (const auto& component : plan.components) {
        const auto& target = component.target;
        if (target.provider != "llamacpp") {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: plan contains a non-llama.cpp target");
        }

        const bool automatic =
            target.semantics == TargetSemantics::BackendAutomatic &&
            target.binding_strength == BindingStrength::Automatic &&
            target.target_id.empty();

        if (automatic) {
            continue;
        }

        if (target.semantics != TargetSemantics::LogicalEndpoint) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: explicit targets must use "
                "LogicalEndpoint semantics");
        }
        if (target.binding_strength != BindingStrength::Exact) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: llama.cpp endpoint targets require "
                "Exact binding");
        }
        if (target.discovery_mode != DiscoveryMode::Configured &&
            target.discovery_mode != DiscoveryMode::Enumerated) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: explicit endpoint targets must be "
                "configured or enumerated");
        }

        validateEndpointSelector(target.target_id);
        if (selected.has_value() && selected.value() != target.target_id) {
            throw std::invalid_argument(
                "LlamaCppLaunchOptions: components specify conflicting "
                "endpoint selectors");
        }
        selected = target.target_id;
    }

    return selected;
}

}  // namespace

std::optional<std::string> LlamaCppLaunchOptions::hexagonDevices(
    const ExecutionPlan& plan) {
    return selectedEndpoint(plan);
}

nlohmann::ordered_json LlamaCppLaunchOptions::toInitOptions(
    const ExecutionPlan& plan) {
    const auto selector = hexagonDevices(plan);
    if (!selector.has_value()) {
        return nlohmann::ordered_json::object();
    }

    nlohmann::ordered_json options = nlohmann::ordered_json::object();
    options["llamacpp"]["hexagon_devices"] = selector.value();
    return options;
}

}  // namespace execution
}  // namespace qai_forge
