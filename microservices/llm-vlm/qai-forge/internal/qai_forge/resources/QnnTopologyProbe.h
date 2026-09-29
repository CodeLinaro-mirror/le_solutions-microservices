// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// QnnTopologyProbe - Hardware-backed HTP enumeration via the QNN C API
//
// dlopen()s a QNN backend library and calls QnnDevice_GetPlatformInfo to
// enumerate hwDevices[].deviceId. Only QNN is probed directly; GenIE and
// llama.cpp have no equivalent enumeration API, so their native IDs are
// seeded from StandardTopologies' documented assumption instead.
//
// This header has NO QNN SDK dependency - QNN types are hidden behind the
// Impl in the .cpp file (dlopen-based), so this header can be included and
// QnnTopologyProbe can be linked into any build regardless of
// QAI_FORGE_BUILD_QNN.
// -----------------------------------------------------------------------------

#pragma once

#include <string>

#include "qai_forge/resources/ResourceTypes.h"

namespace qai_forge {
namespace resources {

struct QnnProbeResult {
    bool succeeded = false;
    std::string error;               // populated iff !succeeded
    PlatformTopology topology;       // populated iff succeeded
};

class QnnTopologyProbe {
public:
    // `backend_lib` is the path to a QNN backend library, e.g.
    // "/usr/lib/libQnnHtp.so".
    static QnnProbeResult probe(const std::string& backend_lib);
};

}  // namespace resources
}  // namespace qai_forge
