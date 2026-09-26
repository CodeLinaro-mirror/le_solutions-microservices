// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/resources/StandardTopologies.h"

namespace qai_forge {
namespace resources {

PlatformTopology makeIQ9Topology() {
    PlatformTopology topo;
    topo.platform_name = "IQ9-QCS9075";
    topo.schema_version = "1.0";

    ResourceNode cpu;
    cpu.id = ResourceId("cpu:0");
    cpu.kind = ResourceKind::Cpu;
    cpu.capacity_units = 1;
    topo.nodes.push_back(cpu);

    ResourceNode gpu;
    gpu.id = ResourceId("gpu:0");
    gpu.kind = ResourceKind::Gpu;
    gpu.capacity_units = 1;
    topo.nodes.push_back(gpu);

    ResourceNode mem;
    mem.id = ResourceId("system-memory");
    mem.kind = ResourceKind::Memory;
    mem.capacity_units = 1;
    topo.nodes.push_back(mem);

    ResourceNode cdsp;
    cdsp.id = ResourceId("cdsp:0");
    cdsp.kind = ResourceKind::AcceleratorDomain;
    cdsp.capacity_units = 1;
    topo.nodes.push_back(cdsp);

    ResourceNode htp0;
    htp0.id = ResourceId("htp:0");
    htp0.kind = ResourceKind::Htp;
    htp0.parent = ResourceId("cdsp:0");
    htp0.capacity_units = 1;
    htp0.native_ids["qnn"] = "0";
    htp0.native_ids["genie"] = "0";
    htp0.native_ids["llamacpp"] = "HTP0:0";
    topo.nodes.push_back(htp0);

    ResourceNode htp1;
    htp1.id = ResourceId("htp:1");
    htp1.kind = ResourceKind::Htp;
    htp1.parent = ResourceId("cdsp:0");
    htp1.capacity_units = 1;
    htp1.native_ids["qnn"] = "1";
    htp1.native_ids["genie"] = "1";
    htp1.native_ids["llamacpp"] = "HTP1:0";
    topo.nodes.push_back(htp1);

    return topo;
}

PlatformTopology makeVentunoQTopology() {
    PlatformTopology topo;
    topo.platform_name = "VentunoQ";
    topo.schema_version = "1.0";

    ResourceNode cpu;
    cpu.id = ResourceId("cpu:0");
    cpu.kind = ResourceKind::Cpu;
    cpu.capacity_units = 1;
    topo.nodes.push_back(cpu);

    ResourceNode mem;
    mem.id = ResourceId("system-memory");
    mem.kind = ResourceKind::Memory;
    mem.capacity_units = 1;
    topo.nodes.push_back(mem);

    ResourceNode cdsp;
    cdsp.id = ResourceId("cdsp:0");
    cdsp.kind = ResourceKind::AcceleratorDomain;
    cdsp.capacity_units = 1;
    topo.nodes.push_back(cdsp);

    ResourceNode htp0;
    htp0.id = ResourceId("htp:0");
    htp0.kind = ResourceKind::Htp;
    htp0.parent = ResourceId("cdsp:0");
    htp0.capacity_units = 1;
    htp0.native_ids["qnn"] = "0";
    htp0.native_ids["genie"] = "0";
    htp0.native_ids["llamacpp"] = "HTP0:0";
    topo.nodes.push_back(htp0);

    return topo;
}

}  // namespace resources
}  // namespace qai_forge
