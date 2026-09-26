// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// Real implementation, compiled only when QAI_FORGE_BUILD_QNN is ON.

#include "qai_forge/resources/QnnTopologyProbe.h"

#include <cstdint>
#include <string>
#include <utility>

#include <dlfcn.h>

#include <QnnInterface.h>

namespace qai_forge {
namespace resources {

namespace {
using QnnInterfaceGetProvidersFn = decltype(QnnInterface_getProviders);
}  // namespace

QnnProbeResult QnnTopologyProbe::probe(const std::string& backend_lib) {
    QnnProbeResult result;

    void* handle = dlopen(backend_lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        result.succeeded = false;
        result.error = "Failed to dlopen '" + backend_lib + "': " +
                       (dlerror() ? dlerror() : "unknown error");
        return result;
    }

    auto* getProviders = reinterpret_cast<QnnInterfaceGetProvidersFn*>(
        dlsym(handle, "QnnInterface_getProviders"));
    if (!getProviders) {
        result.succeeded = false;
        result.error = "Symbol QnnInterface_getProviders not found in '" +
                       backend_lib + "'";
        dlclose(handle);
        return result;
    }

    const QnnInterface_t** providers = nullptr;
    uint32_t num_providers = 0;
    if (getProviders(&providers, &num_providers) != QNN_SUCCESS ||
        !providers || num_providers == 0) {
        result.succeeded = false;
        result.error = "QnnInterface_getProviders failed or returned no providers";
        dlclose(handle);
        return result;
    }

    QNN_INTERFACE_VER_TYPE api = providers[0]->QNN_INTERFACE_VER_NAME;

    if (!api.deviceGetPlatformInfo) {
        result.succeeded = false;
        result.error =
            "Backend does not expose deviceGetPlatformInfo (unsupported "
            "feature for this backend type)";
        dlclose(handle);
        return result;
    }

    const QnnDevice_PlatformInfo_t* platform_info = nullptr;
    Qnn_ErrorHandle_t status = api.deviceGetPlatformInfo(nullptr, &platform_info);

    if (status == QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE) {
        result.succeeded = false;
        result.error = "deviceGetPlatformInfo unsupported by this backend";
        dlclose(handle);
        return result;
    }
    if (status != QNN_SUCCESS || !platform_info) {
        result.succeeded = false;
        result.error =
            "deviceGetPlatformInfo failed, QNN error code=" +
            std::to_string(status);
        dlclose(handle);
        return result;
    }

    PlatformTopology topo;
    topo.platform_name = "qnn-probed";
    topo.schema_version = "1.0";

    ResourceNode cdsp;
    cdsp.id = ResourceId("cdsp:0");
    cdsp.kind = ResourceKind::AcceleratorDomain;
    cdsp.capacity_units = 1;
    topo.nodes.push_back(cdsp);

    const uint32_t num_hw = platform_info->v1.numHwDevices;
    for (uint32_t i = 0; i < num_hw; ++i) {
        const QnnDevice_HardwareDeviceInfo_t& hw = platform_info->v1.hwDevices[i];

        ResourceNode node;
        node.id = ResourceId("htp:" + std::to_string(hw.v1.deviceId));
        node.kind = ResourceKind::Htp;
        node.parent = ResourceId("cdsp:0");
        node.capacity_units = 1;

        // All three providers documented to address the same physical HTP
        // via the same deviceId - matches the assumption already used by
        // StandardTopologies for the non-probed IQ9/VentunoQ topologies.
        const std::string device_id_str = std::to_string(hw.v1.deviceId);
        node.native_ids["qnn"] = device_id_str;
        node.native_ids["genie"] = device_id_str;
        node.native_ids["llamacpp"] = "HTP" + device_id_str + ":0";
        node.attributes["qnn_device_type"] = std::to_string(hw.v1.deviceType);
        node.attributes["qnn_num_cores"] = std::to_string(hw.v1.numCores);

        topo.nodes.push_back(std::move(node));
    }

    if (api.deviceFreePlatformInfo) {
        api.deviceFreePlatformInfo(nullptr, platform_info);
    }

    dlclose(handle);

    result.succeeded = true;
    result.topology = std::move(topo);
    return result;
}

}  // namespace resources
}  // namespace qai_forge
