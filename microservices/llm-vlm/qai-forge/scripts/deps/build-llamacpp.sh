#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# build-llamacpp.sh
#
# Builds llama.cpp with shared libraries for CPU-only inference.
# This is the plain build without Hexagon NPU support.
# ============================================================================

DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"
LLAMACPP_VERSION="${LLAMACPP_VERSION:-b5616}"
LLAMACPP_SRC_DIR="${LLAMACPP_SRC_DIR:-/build/llama.cpp}"
LLAMACPP_INSTALL_DIR="${LLAMACPP_INSTALL_DIR:-/build/llamacpp-install}"

echo "=== Building llama.cpp ${LLAMACPP_VERSION} (CPU-only) ==="

# Clone llama.cpp
git clone --depth 1 --branch "${LLAMACPP_VERSION}" \
    https://github.com/ggml-org/llama.cpp "${LLAMACPP_SRC_DIR}"

# Configure with shared libraries
cmake -S "${LLAMACPP_SRC_DIR}" -B "${LLAMACPP_SRC_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DLLAMA_BUILD_SERVER=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_CURL=OFF \
    -DGGML_NATIVE=OFF \
    -DCMAKE_INSTALL_PREFIX="${LLAMACPP_INSTALL_DIR}"

# Build
cmake --build "${LLAMACPP_SRC_DIR}/build" -j$(nproc)

# Install
cmake --install "${LLAMACPP_SRC_DIR}/build"

# Copy artifacts to deploy directory
mkdir -p "${DEPLOY_DIR}/usr/bin" "${DEPLOY_DIR}/usr/lib"
cp "${LLAMACPP_INSTALL_DIR}/bin/llama-server" "${DEPLOY_DIR}/usr/bin/"
find "${LLAMACPP_INSTALL_DIR}/lib" -name "*.so*" -exec cp -P {} "${DEPLOY_DIR}/usr/lib/" \;

# Copy headers to system include path (for qai-forge build)
mkdir -p /usr/include/ggml/include
cp -r "${LLAMACPP_SRC_DIR}/ggml/include/"* /usr/include/ggml/include/
cp -r "${LLAMACPP_SRC_DIR}/include/"* /usr/include/ 2>/dev/null || true

# Cleanup
rm -rf "${LLAMACPP_SRC_DIR}"

echo "✓ llama.cpp ${LLAMACPP_VERSION} installed"
