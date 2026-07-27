#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# qai-forge/scripts/deps/build-fastrpc.sh
#
# Build fastrpc from source (github.com/quic/fastrpc).
#
# Environment Variables:
#   SKIP_FASTRPC=true       Skip build if already installed (platform package)
#   DEPLOY_DIR              Where to stage runtime libs (default: /build/deploy)
#   DEBUG=1                 Enable debug output (set -x)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# Enable debug mode if requested
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# ─────────────────────────────────────────────────────────────────────────────
# Skip if fastrpc is already installed (platform package)
# ─────────────────────────────────────────────────────────────────────────────
if [ "${SKIP_FASTRPC:-false}" = "true" ]; then
    echo "=== Skipping fastrpc build (SKIP_FASTRPC=true) ==="
    echo "    Assuming libcdsprpc.so / libadsprpc.so are already installed."
    exit 0
fi

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"

echo "=== Building fastrpc from source ==="

# ─────────────────────────────────────────────────────────────────────────────
# Clone fastrpc
# ─────────────────────────────────────────────────────────────────────────────
git clone --depth 1 https://github.com/quic/fastrpc.git /tmp/fastrpc

cd /tmp/fastrpc

# ─────────────────────────────────────────────────────────────────────────────
# Build with custom MACHINE_NAME_PATH for Qualcomm Linux devices
# ─────────────────────────────────────────────────────────────────────────────
autoreconf -is

CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
    ./configure --prefix=/usr --libdir=/usr/lib

make -j$(nproc)
make install

# ─────────────────────────────────────────────────────────────────────────────
# Copy remote.h header (needed by Hexagon SDK builds)
# ─────────────────────────────────────────────────────────────────────────────
find /tmp/fastrpc -name 'remote.h' -exec cp {} /usr/include/ \;

# ─────────────────────────────────────────────────────────────────────────────
# Stage runtime libraries to deploy directory
# ─────────────────────────────────────────────────────────────────────────────
mkdir -p "${DEPLOY_DIR}/usr/lib"
cp -d /usr/lib/libcdsprpc.so* "${DEPLOY_DIR}/usr/lib/" || true
cp -d /usr/lib/libadsprpc.so* "${DEPLOY_DIR}/usr/lib/" || true

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
rm -rf /tmp/fastrpc

echo "✓ fastrpc build complete"
echo "  Runtime libs staged to: ${DEPLOY_DIR}/usr/lib/"
