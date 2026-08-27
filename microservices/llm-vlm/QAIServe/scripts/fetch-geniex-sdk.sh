#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# fetch-geniex-sdk.sh
#
# Downloads and installs the GenieX SDK (libgeniex.so + headers).
# GenieX provides model-manager C ABI for multi-hub model downloads
# (HuggingFace / AI Hub / ...).
# ============================================================================

GENIEX_SDK_VERSION="${GENIEX_SDK_VERSION:-v0.3.14}"
GENIEX_DOWNLOAD_DIR="${GENIEX_DOWNLOAD_DIR:-/tmp/geniex-download}"
DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"

echo "=== Fetching GenieX SDK ${GENIEX_SDK_VERSION} ==="

# Create directories
mkdir -p "${GENIEX_DOWNLOAD_DIR}"

# Download GenieX SDK
wget -t 2 -T 60 -P "${GENIEX_DOWNLOAD_DIR}" \
    "https://github.com/qualcomm/GenieX/releases/download/${GENIEX_SDK_VERSION}/geniex-sdk-linux-arm64-${GENIEX_SDK_VERSION}.zip"

# Extract
cd "${GENIEX_DOWNLOAD_DIR}"
unzip "geniex-sdk-linux-arm64-${GENIEX_SDK_VERSION}.zip"

# Copy headers to system include path
cp sdk-linux-arm64/include/*.h /usr/include/

# Copy libgeniex.so to system lib path (for build)
cp sdk-linux-arm64/lib/libgeniex.so /usr/lib/

# Copy libgeniex.so and backends to deploy directory (for runtime)
cp sdk-linux-arm64/lib/libgeniex.so "${DEPLOY_DIR}/usr/lib/"
cp sdk-linux-arm64/lib/qairt/libgeniex*.so "${DEPLOY_DIR}/usr/lib/"
cp sdk-linux-arm64/lib/qairt/libgeniex-proc*.so "${DEPLOY_DIR}/usr/lib/" 2>/dev/null || true

# Cleanup
rm -rf "${GENIEX_DOWNLOAD_DIR}/sdk-linux-arm64" "${GENIEX_DOWNLOAD_DIR}"/*.zip

echo "✓ GenieX SDK ${GENIEX_SDK_VERSION} installed"
