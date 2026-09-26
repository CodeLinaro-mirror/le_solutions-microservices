// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/QnnLaunchOptions.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace qai_forge {
namespace execution {
namespace {

uint32_t parseDeviceId(const std::string& target_id) {
    const std::string prefix = "device_id=";
    const std::string value =
        target_id.rfind(prefix, 0) == 0
            ? target_id.substr(prefix.size())
            : target_id;

    if (value.empty()) {
        throw std::invalid_argument(
            "QnnLaunchOptions: QNN target has an empty device ID");
    }

    size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &consumed, 10);
    } catch (...) {
        throw std::invalid_argument(
            "QnnLaunchOptions: invalid QNN device target '" + target_id + "'");
    }

    if (consumed != value.size() ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(
            "QnnLaunchOptions: invalid QNN device target '" + target_id + "'");
    }

    return static_cast<uint32_t>(parsed);
}

}  // namespace

std::optional<uint32_t> QnnLaunchOptions::selectedDeviceId(
    const ExecutionPlan& plan) {
    if (plan.provider.empty()) {
        throw std::invalid_argument(
            "QnnLaunchOptions: plan has an empty provider");
    }

    if (plan.provider != "qnn") {
        throw std::invalid_argument(
            "QnnLaunchOptions: expected qnn plan, got '" + plan.provider + "'");
    }
    if (plan.components.empty()) {
        throw std::invalid_argument(
            "QnnLaunchOptions: QNN plan has no components");
    }

    std::optional<uint32_t> selected;
    bool saw_automatic = false;
    bool saw_target = false;
    for (const auto& component : plan.components) {
        if (component.target.provider != "qnn") {
            throw std::invalid_argument(
                "QnnLaunchOptions: component provider is not qnn");
        }

        if (component.target.semantics ==
            TargetSemantics::BackendAutomatic) {
            saw_automatic = true;
            if (saw_target) {
                throw std::invalid_argument(
                    "QnnLaunchOptions: exact and automatic targets cannot be mixed");
            }
            continue;
        }

        saw_target = true;
        if (saw_automatic) {
            throw std::invalid_argument(
                "QnnLaunchOptions: exact and automatic targets cannot be mixed");
        }

        if (component.target.semantics != TargetSemantics::PhysicalInstance ||
            component.target.binding_strength != BindingStrength::Exact) {
            throw std::invalid_argument(
                "QnnLaunchOptions: QNN launch requires exact physical targets");
        }

        const uint32_t device_id = parseDeviceId(component.target.target_id);
        if (selected.has_value() && *selected != device_id) {
            throw std::invalid_argument(
                "QnnLaunchOptions: components request conflicting device IDs");
        }
        selected = device_id;
    }

    return selected;
}

nlohmann::ordered_json QnnLaunchOptions::toInitOptions(
    const ExecutionPlan& plan) {
    const auto selected = selectedDeviceId(plan);
    if (!selected.has_value()) {
        return nlohmann::ordered_json::object();
    }

    return {
        {"qnn", {
            {"device_id", *selected},
            {"require_exact", true}
        }}
    };
}

}  // namespace execution
}  // namespace qai_forge
