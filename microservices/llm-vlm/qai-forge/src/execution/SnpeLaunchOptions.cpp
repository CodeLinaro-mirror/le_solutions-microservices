// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/SnpeLaunchOptions.h"

#include <stdexcept>
#include <string>

namespace qai_forge {
namespace execution {

namespace {

const ComponentPlacement* findRuntimePreference(
    const ExecutionPlan& plan) {
    const ComponentPlacement* preference = nullptr;

    for (const auto& component : plan.components) {
        if (component.target.provider != "snpe") {
            throw std::invalid_argument(
                "SnpeLaunchOptions: plan contains a non-SNPE target");
        }

        if (component.target.semantics ==
            TargetSemantics::RuntimePreference) {
            if (preference != nullptr) {
                throw std::invalid_argument(
                    "SnpeLaunchOptions: multiple runtime-preference targets "
                    "are not supported");
            }
            if (component.target.discovery_mode !=
                DiscoveryMode::Configured) {
                throw std::invalid_argument(
                    "SnpeLaunchOptions: runtime preference must be configured");
            }
            if (component.target.binding_strength !=
                BindingStrength::Preferred) {
                throw std::invalid_argument(
                    "SnpeLaunchOptions: runtime preference must be preferred");
            }
            preference = &component;
        } else if (component.target.semantics !=
                   TargetSemantics::BackendAutomatic) {
            throw std::invalid_argument(
                "SnpeLaunchOptions: unsupported target semantics for SNPE");
        }
    }

    return preference;
}

std::string extractRuntimeOrder(const std::string& target_id) {
    constexpr const char* prefix = "runtime_order=";
    if (target_id.rfind(prefix, 0) != 0) {
        throw std::invalid_argument(
            "SnpeLaunchOptions: target must use runtime_order=<value>");
    }

    const std::string value = target_id.substr(std::string(prefix).size());
    if (value.empty()) {
        throw std::invalid_argument(
            "SnpeLaunchOptions: runtime_order must not be empty");
    }
    return value;
}

}  // namespace

std::string SnpeLaunchOptions::runtimeOrder(const ExecutionPlan& plan) {
    if (plan.provider != "snpe") {
        throw std::invalid_argument(
            "SnpeLaunchOptions: expected an SNPE execution plan");
    }

    const auto* preference = findRuntimePreference(plan);
    if (preference == nullptr) {
        return {};
    }

    if (preference->target.binding_strength == BindingStrength::Exact) {
        throw std::invalid_argument(
            "SnpeLaunchOptions: SNPE cannot guarantee exact binding");
    }

    return extractRuntimeOrder(preference->target.target_id);
}

nlohmann::ordered_json SnpeLaunchOptions::toInitOptions(
    const ExecutionPlan& plan) {
    const std::string order = runtimeOrder(plan);
    if (order.empty()) {
        return nlohmann::ordered_json::object();
    }

    return nlohmann::ordered_json{
        {"snpe", {{"runtime_order", order}}}};
}

}  // namespace execution
}  // namespace qai_forge
