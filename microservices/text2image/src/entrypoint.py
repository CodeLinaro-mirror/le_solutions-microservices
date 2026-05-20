#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# entrypoint.py
# Runs at container startup (as root) to:
#   1. Detect the real GIDs of fastrpc and dmaheap devices on the host platform
#   2. Detect the UID/GID of the mounted models directory
#   3. Add target user to those supplementary groups dynamically
#   4. Drop privileges to the target user and exec the container command

import os
import subprocess
import sys

def get_gid(path):
    try:
        return os.stat(path).st_gid
    except FileNotFoundError:
        return None

def main():
    print("[entrypoint] Starting Python-based group ID configuration...", flush=True)

    # 1. Detect GIDs for DSP and DMA devices dynamically
    devices = [
        "/dev/fastrpc-cdsp", "/dev/fastrpc-cdsp1",
        "/dev/dma_heap/system", "/dev/dma_heap/qcom,system"
    ]

    supplementary_gids = set()
    for dev in devices:
        gid = get_gid(dev)
        if gid is not None:
            print(f"[entrypoint] Found {dev} owned by host GID {gid}", flush=True)
            supplementary_gids.add(gid)

    # 2. Detect target UID/GID from models directory, or fallback to 10000
    TARGET_UID = 10000
    TARGET_GID = 10000
    models_dir = "/opt/image_gen"
    if os.path.exists(models_dir):
        stat_info = os.stat(models_dir)
        TARGET_UID = stat_info.st_uid
        TARGET_GID = stat_info.st_gid
        print(f"[entrypoint] Detected {models_dir} owned by UID {TARGET_UID}, GID {TARGET_GID}. Adopting these IDs.", flush=True)
    else:
        print(f"[entrypoint] Directory {models_dir} not found. Defaulting to UID {TARGET_UID}, GID {TARGET_GID}.", flush=True)

    # Ensure the user's primary GID is in the supplementary groups list
    supplementary_gids.add(TARGET_GID)

    # 3. Drop privileges dynamically
    if os.getuid() == 0:
        print(f"[entrypoint] Dropping privileges from root to UID {TARGET_UID} / GID {TARGET_GID}...", flush=True)
        try:
            os.setgroups(list(supplementary_gids))
            os.setresgid(TARGET_GID, TARGET_GID, TARGET_GID)
            os.setresuid(TARGET_UID, TARGET_UID, TARGET_UID)
        except OSError as e:
            print(f"[entrypoint] FATAL: Failed to drop privileges: {e}", flush=True)
            sys.exit(1)

    # 4. Execute the target container command
    if len(sys.argv) > 1:
        cmd = sys.argv[1:]
        print(f"[entrypoint] Executing application: {' '.join(cmd)}", flush=True)
        result = subprocess.run(cmd, shell=False)
        sys.exit(result.returncode)
    else:
        print("[entrypoint] No application command provided.", flush=True)

if __name__ == "__main__":
    main()
