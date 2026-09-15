#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# entrypoint.py
# Runs at container startup (as root) to:
#   1. Detect the real GIDs of fastrpc and dmaheap devices on the host platform
#   2. Detect the UID/GID of the mounted models directory
#   3. Add target user to those supplementary groups dynamically
#   4. Merge the read-only host qcom config into a private, writable qcom
#      directory (without ever writing back to the host)
#   5. Drop privileges to the target user and exec the container command

import glob
import os
import shutil
import subprocess
import sys

def get_gid(path):
    try:
        return os.stat(path).st_gid
    except FileNotFoundError:
        return None

def setup_qcom_conf():
    """
    Build a private, writable /usr/share/qcom inside the container from the
    read-only host mount (HOST_QCOM, default /run/host-qcom), then layer our
    own baked fastrpc DSP config on top.

    We intentionally never bind-mount /usr/share/qcom directly onto itself
    (read-write) because that would let the container write into the host's
    real config directory. Instead HOST_QCOM is mounted read-only, and we
    reconstruct a container-local QCOM directory here:
      - Top-level entries (except conf.d) are symlinked from HOST_QCOM into
        QCOM, so any host-provided libraries/binaries remain visible.
      - conf.d entries must be regular files -- fastrpc skips symlinks when
        scanning conf.d -- so *.yaml/*.yml files are copied instead.
      - Our own baked config (/etc/fastrpc/hexagon-dsp-binaries.yaml, built
        into the image) is copied in last as 00-hexagon-dsp-binaries.yaml.
    """
    os.umask(0o022)

    host_qcom = os.environ.get("HOST_QCOM", "/run/host-qcom")
    qcom = os.environ.get("QCOM", "/usr/share/qcom")

    try:
        os.makedirs(os.path.join(qcom, "conf.d"), exist_ok=True)
    except OSError as e:
        print(f"[entrypoint] WARNING: Could not create {qcom}/conf.d: {e}", flush=True)
        return

    if os.path.isdir(host_qcom):
        for name in os.listdir(host_qcom):
            if name == "conf.d":
                continue
            src = os.path.join(host_qcom, name)
            dst = os.path.join(qcom, name)
            try:
                if os.path.islink(dst) or os.path.exists(dst):
                    if os.path.isdir(dst) and not os.path.islink(dst):
                        shutil.rmtree(dst)
                    else:
                        os.remove(dst)
                os.symlink(src, dst)
            except OSError as e:
                print(f"[entrypoint] WARNING: Could not symlink {src} -> {dst}: {e}", flush=True)

        host_confd = os.path.join(host_qcom, "conf.d")
        if os.path.isdir(host_confd):
            for name in os.listdir(host_confd):
                if not name.endswith((".yaml", ".yml")):
                    continue
                src = os.path.join(host_confd, name)
                if os.path.isfile(src):
                    try:
                        shutil.copy2(src, os.path.join(qcom, "conf.d", name))
                    except OSError as e:
                        print(f"[entrypoint] WARNING: Could not copy {src}: {e}", flush=True)
    else:
        print(f"[entrypoint] {host_qcom} not present; skipping host qcom merge.", flush=True)

    fastrpc_src = "/etc/fastrpc/hexagon-dsp-binaries.yaml"
    if os.path.isfile(fastrpc_src):
        try:
            dst = os.path.join(qcom, "conf.d", "00-hexagon-dsp-binaries.yaml")
            shutil.copy2(fastrpc_src, dst)
            print(f"[entrypoint] Deployed {fastrpc_src} -> {dst}", flush=True)
        except OSError as e:
            print(f"[entrypoint] WARNING: Could not deploy fastrpc config: {e}", flush=True)

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

    # Detect GID of the read-only host qcom mount so the process inherits the
    # same group access as seen on the host.
    host_qcom = os.environ.get("HOST_QCOM", "/run/host-qcom")
    host_qcom_gid = get_gid(host_qcom)
    if host_qcom_gid is not None:
        print(f"[entrypoint] Found {host_qcom} owned by host GID {host_qcom_gid}", flush=True)
        supplementary_gids.add(host_qcom_gid)

    # 2. Detect target UID/GID from models directory, or fallback to 10000
    TARGET_UID = 10000
    TARGET_GID = 10000
    models_dir = "/mnt/work/models"
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
        # Fix device permissions so the dropped-privilege process can open them.
        # Mirrors the `chmod 666 /dev/fastrpc* /dev/dma_heap/*` done in the
        # reference compose command block.
        for pattern in ["/dev/fastrpc*", "/dev/dma_heap/*"]:
            for dev_path in glob.glob(pattern):
                try:
                    os.chmod(dev_path, 0o666)
                    print(f"[entrypoint] Set permissions 666 on {dev_path}", flush=True)
                except OSError as e:
                    print(f"[entrypoint] WARNING: Could not chmod {dev_path}: {e}", flush=True)

        # Merge the read-only host qcom config into our private, writable
        # qcom directory, layering in our own baked fastrpc DSP config, while
        # we still have root privileges.
        setup_qcom_conf()

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
