// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Standalone CLI for hardware topology discovery.
//
// Usage:
//   topology-probe [--qnn-backend-lib PATH] [--platform iq9|ventunoq]

#include <cstring>
#include <iostream>
#include <string>

#include "qai_forge/resources/QnnTopologyProbe.h"
#include "qai_forge/resources/StandardTopologies.h"
#include "qai_forge/resources/TopologyRegistry.h"

using qai_forge::resources::PlatformTopology;
using qai_forge::resources::QnnProbeResult;
using qai_forge::resources::QnnTopologyProbe;
using qai_forge::resources::TopologyRegistry;
using qai_forge::resources::makeIQ9Topology;
using qai_forge::resources::makeVentunoQTopology;

namespace {

void printUsage() {
    std::cerr <<
        "Usage: topology-probe [--qnn-backend-lib PATH] "
        "[--platform iq9|ventunoq]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string qnn_backend_lib;
    std::string platform = "iq9";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--qnn-backend-lib") == 0 && i + 1 < argc) {
            qnn_backend_lib = argv[++i];
        } else if (std::strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            platform = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            printUsage();
            return 1;
        }
    }

    PlatformTopology topology;
    bool hardware_verified = false;
    std::string probe_error;

    if (!qnn_backend_lib.empty()) {
        QnnProbeResult probe_result = QnnTopologyProbe::probe(qnn_backend_lib);
        if (probe_result.succeeded) {
            topology = std::move(probe_result.topology);
            hardware_verified = true;
        } else {
            probe_error = probe_result.error;
            std::cerr << "[topology-probe] QNN probe failed: "
                      << probe_result.error << "\n";
            std::cerr << "[topology-probe] Falling back to static topology "
                         "for platform '" << platform << "'.\n";
        }
    }

    if (!hardware_verified) {
        if (platform == "ventunoq") {
            topology = makeVentunoQTopology();
        } else if (platform == "iq9") {
            topology = makeIQ9Topology();
        } else {
            std::cerr << "[topology-probe] Unknown --platform value '"
                      << platform << "'. Expected 'iq9' or 'ventunoq'.\n";
            printUsage();
            return 1;
        }
        std::cerr << "[topology-probe] WARNING: topology is a static "
                     "assumption, not hardware-verified.\n";
    }

    try {
        auto registry = TopologyRegistry::create(topology);
        std::string topology_json = registry->toJson();

        std::cout << "{\n";
        std::cout << "  \"probe_source\": \""
                   << (hardware_verified ? "qnn-enumerated" : "static-fallback")
                   << "\",\n";
        std::cout << "  \"requested_platform\": \"" << platform << "\",\n";
        std::cout << "  \"qnn_backend_lib\": \""
                   << (qnn_backend_lib.empty() ? "" : qnn_backend_lib) << "\",\n";
        if (!hardware_verified && !qnn_backend_lib.empty()) {
            std::cout << "  \"qnn_probe_error\": \"" << probe_error << "\",\n";
        }
        std::cout << "  \"topology\": " << topology_json;
        std::cout << "}\n";
    } catch (const std::exception& e) {
        std::cerr << "[topology-probe] Invalid topology: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
