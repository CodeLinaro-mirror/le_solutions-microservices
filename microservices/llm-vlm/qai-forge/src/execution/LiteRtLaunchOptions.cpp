// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/LiteRtLaunchOptions.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qai_forge {
namespace execution {

namespace {

constexpr int kCpu = 1;
constexpr int kGpu = 2;
constexpr int kNpu = 4;
constexpr int kDefaultAccelerators = kNpu | kCpu;

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [](unsigned char c) {
                                                return !std::isspace(c);
                                            }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [](unsigned char c) {
                                 return !std::isspace(c);
                             }).base(),
                value.end());
    return value;
}

std::string uppercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::toupper(c));
                   });
    return value;
}

int parseAccelerators(const std::string& target_id) {
    constexpr const char* prefix = "accelerators=";
    std::string value = target_id;
    if (value.rfind(prefix, 0) == 0) {
        value = value.substr(std::string(prefix).size());
    }
    if (value.empty()) {
        throw std::invalid_argument(
            "LiteRtLaunchOptions: accelerator class set must not be empty");
    }

    int mask = 0;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t separator = value.find('|', start);
        const std::string token = uppercase(trim(value.substr(
            start, separator == std::string::npos ? std::string::npos
                                                   : separator - start)));
        if (token == "CPU") {
            mask |= kCpu;
        } else if (token == "GPU") {
            mask |= kGpu;
        } else if (token == "NPU") {
            mask |= kNpu;
        } else if (token.empty()) {
            throw std::invalid_argument(
                "LiteRtLaunchOptions: accelerator class set contains an "
                "empty entry");
        } else {
            throw std::invalid_argument(
                "LiteRtLaunchOptions: unsupported accelerator class '" +
                token + "'");
        }

        if (separator == std::string::npos) break;
        start = separator + 1;
    }

    if (mask == 0) {
        throw std::invalid_argument(
            "LiteRtLaunchOptions: accelerator class set is empty");
    }
    return mask;
}

const ComponentPlacement* findClassSet(const ExecutionPlan& plan) {
    const ComponentPlacement* class_set = nullptr;

    for (const auto& component : plan.components) {
        if (component.target.provider != "litert") {
            throw std::invalid_argument(
                "LiteRtLaunchOptions: plan contains a non-LiteRT target");
        }

        if (component.target.semantics ==
            TargetSemantics::AcceleratorClassSet) {
            if (class_set != nullptr) {
                throw std::invalid_argument(
                    "LiteRtLaunchOptions: multiple accelerator class sets "
                    "are not supported");
            }
            if (component.target.discovery_mode != DiscoveryMode::Configured) {
                throw std::invalid_argument(
                    "LiteRtLaunchOptions: accelerator class set must be "
                    "configured");
            }
            if (component.target.binding_strength != BindingStrength::Automatic) {
                throw std::invalid_argument(
                    "LiteRtLaunchOptions: LiteRT accelerator class sets must "
                    "use automatic binding");
            }
            class_set = &component;
        } else if (component.target.semantics !=
                   TargetSemantics::BackendAutomatic) {
            throw std::invalid_argument(
                "LiteRtLaunchOptions: unsupported target semantics for LiteRT");
        }
    }

    return class_set;
}

}  // namespace

int LiteRtLaunchOptions::hwAccelerators(const ExecutionPlan& plan) {
    if (plan.provider != "litert") {
        throw std::invalid_argument(
            "LiteRtLaunchOptions: expected a LiteRT execution plan");
    }

    const auto* class_set = findClassSet(plan);
    return class_set == nullptr ? kDefaultAccelerators
                                : parseAccelerators(class_set->target.target_id);
}

nlohmann::ordered_json LiteRtLaunchOptions::toInitOptions(
    const ExecutionPlan& plan) {
    if (plan.provider != "litert") {
        throw std::invalid_argument(
            "LiteRtLaunchOptions: expected a LiteRT execution plan");
    }

    const auto* class_set = findClassSet(plan);
    if (class_set == nullptr) {
        return nlohmann::ordered_json::object();
    }

    return nlohmann::ordered_json{
        {"litert",
         {{"hw_accelerators", hwAccelerators(plan)}}}};
}

}  // namespace execution
}  // namespace qai_forge
