// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/LiteRtLmLaunchOptions.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace qai_forge {
namespace execution {

namespace {

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(
        value.begin(), value.end(), [](unsigned char c) {
            return !std::isspace(c);
        }));
    value.erase(std::find_if(
        value.rbegin(), value.rend(), [](unsigned char c) {
            return !std::isspace(c);
        }).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

std::string backendValue(const std::string& target_id) {
    constexpr const char* prefix = "backend=";
    if (target_id.rfind(prefix, 0) != 0) {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: target must use backend=<cpu|gpu|npu>");
    }

    const std::string value =
        lower(trim(target_id.substr(std::string(prefix).size())));
    if (value != "cpu" && value != "gpu" && value != "npu") {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: unsupported backend '" + value + "'");
    }
    return value;
}

void addBackendForComponent(
    nlohmann::ordered_json& backends,
    const ComponentPlacement& component) {
    if (component.target.provider != "litert_lm") {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: plan contains a non-LiteRT-LM target");
    }

    // The current worker passes only the engine-wide main-graph backend to
    // LiteRT-LM's C API. Component-specific selectors that the worker
    // cannot enforce are rejected.
    if (component.role != ComponentRole::MainGraph &&
        !component.target.target_id.empty()) {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: component-specific backend selection is "
            "not supported by the current worker");
    }

    if (component.target.semantics != TargetSemantics::BackendAutomatic) {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: LiteRT-LM supports backend-managed targets "
            "only");
    }

    if (component.target.target_id.empty()) {
        return;
    }

    if (component.target.discovery_mode != DiscoveryMode::Configured &&
        component.target.discovery_mode != DiscoveryMode::BackendManaged) {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: backend target must be configured or "
            "backend-managed");
    }

    const std::string backend = backendValue(component.target.target_id);
    const std::string role = toString(component.role);
    backends[role] = backend;
}

}  // namespace

nlohmann::ordered_json LiteRtLmLaunchOptions::componentBackends(
    const ExecutionPlan& plan) {
    if (plan.provider != "litert_lm") {
        throw std::invalid_argument(
            "LiteRtLmLaunchOptions: expected a LiteRT-LM execution plan");
    }

    nlohmann::ordered_json backends = nlohmann::ordered_json::object();
    for (const auto& component : plan.components) {
        addBackendForComponent(backends, component);
    }
    return backends;
}

nlohmann::ordered_json LiteRtLmLaunchOptions::toInitOptions(
    const ExecutionPlan& plan) {
    const auto backends = componentBackends(plan);
    if (backends.empty()) {
        return nlohmann::ordered_json::object();
    }

    nlohmann::ordered_json options = nlohmann::ordered_json::object();
    options["litert_lm"]["component_backends"] = backends;

    // The current LiteRT-LM worker has one engine-wide backend setting.
    const auto main = backends.find("main_graph");
    if (main != backends.end()) {
        options["litert_lm"]["backend"] = main.value();
    }

    return options;
}

}  // namespace execution
}  // namespace qai_forge
