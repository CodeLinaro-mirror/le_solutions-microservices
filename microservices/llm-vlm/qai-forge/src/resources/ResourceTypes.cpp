// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/resources/ResourceTypes.h"

namespace qai_forge {
namespace resources {

const char* toString(ResourceKind kind) {
    switch (kind) {
        case ResourceKind::Cpu:               return "cpu";
        case ResourceKind::Gpu:               return "gpu";
        case ResourceKind::Htp:               return "htp";
        case ResourceKind::Memory:            return "memory";
        case ResourceKind::AcceleratorDomain: return "accelerator_domain";
        default:                              return "unknown";
    }
}

}  // namespace resources
}  // namespace qai_forge
