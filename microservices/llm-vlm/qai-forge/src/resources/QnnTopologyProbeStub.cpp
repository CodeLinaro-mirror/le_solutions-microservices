// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Compiled instead of QnnTopologyProbe.cpp when QAI_FORGE_BUILD_QNN is OFF.
// Always returns failure so callers fall back to StandardTopologies.

#include "qai_forge/resources/QnnTopologyProbe.h"

namespace qai_forge {
namespace resources {

QnnProbeResult QnnTopologyProbe::probe(const std::string& /*backend_lib*/) {
    QnnProbeResult result;
    result.succeeded = false;
    result.error =
        "QnnTopologyProbe: QNN support was not compiled into this build "
        "(QAI_FORGE_BUILD_QNN=OFF). Rebuild with -DQAI_FORGE_BUILD_QNN=ON "
        "and -DQNN_INCLUDE_PATH=<path> to enable hardware-backed HTP "
        "enumeration.";
    return result;
}

}  // namespace resources
}  // namespace qai_forge
