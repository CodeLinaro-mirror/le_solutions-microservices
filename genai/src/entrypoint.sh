#!/bin/bash
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# entrypoint.sh
# Runs at container startup (as root) to:
#   1. Detect the real GIDs of fastrpc and dmaheap devices on the host platform
#   2. Update the pre-created groups to match those GIDs
#   3. Add iot-user to those groups
#   4. Fix ownership of the models working directory
#   5. Drop privileges to iot-user and exec the container command

set -e

echo "[entrypoint] Starting group ID configuration..."

# ---------------------------------------------------------------------------
# Helper: update a group's GID to match the GID of a device file
# Usage: configure_group <group_name> <device_path>
# ---------------------------------------------------------------------------
configure_group() {
    local group_name="$1"
    local device_path="$2"

    if [ ! -e "${device_path}" ]; then
        echo "[entrypoint] WARNING: device ${device_path} not found, skipping ${group_name} group setup"
        return
    fi

    # Get the GID that owns the device on the host (visible inside the container)
    local host_gid
    host_gid=$(stat -c "%g" "${device_path}")

    echo "[entrypoint] Device ${device_path} has GID ${host_gid} on this platform"

    # Check if a group already exists with this GID (could be a different name)
    local existing_group
    existing_group=$(getent group "${host_gid}" | cut -d: -f1 || true)

    if [ -n "${existing_group}" ] && [ "${existing_group}" != "${group_name}" ]; then
        # Another group already owns this GID; add iot-user to that group instead
        echo "[entrypoint] GID ${host_gid} is already owned by group '${existing_group}', adding iot-user to it"
        usermod -aG "${existing_group}" iot-user
    else
        # Update our pre-created group to use the real host GID
        # -o allows non-unique GIDs in case of overlap
        groupmod -o -g "${host_gid}" "${group_name}"
        echo "[entrypoint] Updated group '${group_name}' to GID ${host_gid}"
        usermod -aG "${group_name}" iot-user
        echo "[entrypoint] Added iot-user to group '${group_name}' (GID ${host_gid})"
    fi
}

# ---------------------------------------------------------------------------
# Configure fastrpc group
# Both fastrpc-cdsp and fastrpc-cdsp1 share the same fastrpc group GID.
# Check each device node independently so platforms with both nodes are
# handled correctly (the second call is effectively a no-op when the GID
# and group membership are already set by the first).
# ---------------------------------------------------------------------------
FASTRPC_FOUND=0
if [ -e "/dev/fastrpc-cdsp" ]; then
    configure_group fastrpc /dev/fastrpc-cdsp
    FASTRPC_FOUND=1
fi
if [ -e "/dev/fastrpc-cdsp1" ]; then
    configure_group fastrpc /dev/fastrpc-cdsp1
    FASTRPC_FOUND=1
fi
if [ "${FASTRPC_FOUND}" -eq 0 ]; then
    echo "[entrypoint] WARNING: No fastrpc device found (/dev/fastrpc-cdsp or /dev/fastrpc-cdsp1)"
fi

# ---------------------------------------------------------------------------
# Configure dmaheap group
# Check each dma_heap device node independently; both nodes share the same
# dmaheap group GID across platforms.
# ---------------------------------------------------------------------------
DMAHEAP_FOUND=0
if [ -e "/dev/dma_heap/system" ]; then
    configure_group dmaheap /dev/dma_heap/system
    DMAHEAP_FOUND=1
fi
if [ -e "/dev/dma_heap/qcom,system" ]; then
    configure_group dmaheap "/dev/dma_heap/qcom,system"
    DMAHEAP_FOUND=1
fi
if [ "${DMAHEAP_FOUND}" -eq 0 ]; then
    echo "[entrypoint] WARNING: No dma_heap device found (/dev/dma_heap/system or /dev/dma_heap/qcom,system)"
fi

# ---------------------------------------------------------------------------
# Fix ownership of the models working directory so iot-user can access it
# ---------------------------------------------------------------------------
echo "[entrypoint] Setting ownership of /mnt/work/models to iot-user..."
chown -R iot-user:iot-user /mnt/work/models

# ---------------------------------------------------------------------------
# Drop privileges to iot-user and execute the container command
# Using 'runuser' preserves the supplementary groups we just configured
# ---------------------------------------------------------------------------
echo "[entrypoint] Dropping privileges to iot-user and executing: $*"
exec runuser -u iot-user -- "$@"
