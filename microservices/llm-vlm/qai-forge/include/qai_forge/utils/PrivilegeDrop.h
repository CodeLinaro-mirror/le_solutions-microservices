// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

namespace qai_forge {
namespace utils {

/**
 * Drop privileges from root to the target user detected from the mounted
 * models directory. Also:
 *
 *   1. Discovers the host GIDs of all DSP/DMA devices via glob and adds them
 *      as supplementary groups so that inference worker subprocesses can open
 *      /dev/fastrpc-cdsp and /dev/dma_heap/*.
 *   2. Discovers the GID of /usr/share/qcom (bind-mounted from the host) and
 *      adds it as a supplementary group.
 *   3. Sets 0666 permissions on all /dev/fastrpc* and /dev/dma_heap/* devices
 *      while still running as root, so the dropped-privilege process can open
 *      them without needing the host GID to be mapped inside the container.
 *   4. Copies the baked /etc/fastrpc/hexagon-dsp-binaries.yaml config into
 *      /usr/share/qcom/conf.d/ now that the bind-mount is in place.
 *   5. Detects the target UID/GID from /mnt/work/models (fallback: 10000/10000).
 *   6. Calls setgroups(), setresgid(), setresuid() to permanently drop root.
 *
 * Must be called before any threads or sockets are opened.
 *
 * Docker Compose mount points expected at runtime:
 *   devices:
 *     - /dev/dma_heap/system
 *     - /dev/dma_heap/qcom,system
 *     - /dev/fastrpc-cdsp
 *     - /dev/fastrpc-cdsp1
 *   volumes:
 *     - /sys/firmware/devicetree/base/model:/run/device-model
 *     - /usr/share/qcom:/usr/share/qcom
 *     - ${GENAI_MODEL_DIR}:/mnt/work/models/
 */
void drop_privileges_and_bind_devices();

} // namespace utils
} // namespace qai_forge
