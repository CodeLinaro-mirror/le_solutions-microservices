// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// StandardTopologies - Known-platform topology builders
//
// These builders provide static, documented topology shapes used as:
//   - fallback/default data when hardware probing is unavailable;
//   - the numeric correspondence between ResourceId and provider-native IDs
//     is the CURRENT WORKING ASSUMPTION for IQ9-class platforms and requires
//     hardware validation before being treated as verified for any specific
//     SKU/build.
// -----------------------------------------------------------------------------

#pragma once

#include "qai_forge/resources/ResourceTypes.h"

namespace qai_forge {
namespace resources {

// Dragonwing IQ9 / QCS9075-class platform: two physical HTPs sharing one
// CDSP domain, plus CPU/GPU/system-memory nodes.
PlatformTopology makeIQ9Topology();

// VentunoQ-class platform: exactly one physical HTP.
PlatformTopology makeVentunoQTopology();

}  // namespace resources
}  // namespace qai_forge
