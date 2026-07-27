#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# fetch-qairt-sdk.sh
#
# Downloads and installs the Qualcomm AI Runtime Community SDK (QAIRT).
# Supports skipping the download if SKIP_QAIRT=true (for platform packages).
# ============================================================================

# Skip if already installed
if [ "${SKIP_QAIRT:-false}" = "true" ]; then
    echo "=== Skipping QAIRT SDK fetch (SKIP_QAIRT=true) ==="
    exit 0
fi

# Configuration
QAIRT_VERSION="${QAIRT_VERSION:-2.45.40.260406}"
QAIRT_DOWNLOAD_DIR="${QAIRT_DOWNLOAD_DIR:-/tmp/qairt-download}"
DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"

echo "=== Fetching QAIRT SDK v${QAIRT_VERSION} ==="

# Create directories
mkdir -p "${QAIRT_DOWNLOAD_DIR}" \
         "${DEPLOY_DIR}/usr/lib/rfsa/adsp" \
         "${DEPLOY_DIR}/usr/include"

# Download QAIRT SDK (resume on drop; retry with backoff instead of
# restarting the multi-hundred-MB transfer from scratch each time)
wget -c -t 5 -T 30 --waitretry=10 -P "${QAIRT_DOWNLOAD_DIR}" \
    "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip"

# Extract
cd "${QAIRT_DOWNLOAD_DIR}"
unzip "v${QAIRT_VERSION}.zip"
rm -f "v${QAIRT_VERSION}.zip"

# Set paths
QAIRT_LIB_PATH="${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/lib"

# Copy Genie headers to system include path
cp -r "${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/Genie/"* \
    /usr/include/

# Copy QNN headers to system include path (needed by qai-forge's qnn-engine)
cp -r "${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/QNN/"* \
    /usr/include/

# Copy SNPE headers to system include path, if present (needed by
# qai-forge's snpe-engine). SNPE .so files were already stripped above, but
# the headers are still required for callers that build against SNPE.h
# without the runtime library available on this SDK build.
cp -r "${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/SNPE/"* \
    /usr/include/ 2>/dev/null || true

# Copy Genie library to system lib path
cp "${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so" \
    /usr/lib/

# Copy DSP libraries to deploy directory
cp "${QAIRT_LIB_PATH}/hexagon-v68/unsigned/"* "${DEPLOY_DIR}/usr/lib/rfsa/adsp/"
cp "${QAIRT_LIB_PATH}/hexagon-v73/unsigned/"* "${DEPLOY_DIR}/usr/lib/rfsa/adsp/"
cp "${QAIRT_LIB_PATH}/hexagon-v75/unsigned/"* "${DEPLOY_DIR}/usr/lib/rfsa/adsp/"

# Copy all aarch64 libraries to deploy directory
cp "${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/"* \
    "${DEPLOY_DIR}/usr/lib/"

# Cleanup
rm -rf "${QAIRT_DOWNLOAD_DIR}/qairt"

echo "✓ QAIRT SDK v${QAIRT_VERSION} installed"
