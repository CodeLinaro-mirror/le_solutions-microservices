#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# build-drogon.sh
#
# Builds Drogon HTTP framework from source with minimal dependencies.
# Disables all optional database/cache backends (PostgreSQL, MySQL, SQLite3,
# Redis, Brotli) to avoid unnecessary dependencies.
# ============================================================================

DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"
DROGON_VERSION="${DROGON_VERSION:-v1.9.6}"

echo "=== Building Drogon ${DROGON_VERSION} ==="

# Clone Drogon
git clone --depth 1 --branch "${DROGON_VERSION}" \
    https://github.com/drogonframework/drogon.git /tmp/drogon

cd /tmp/drogon
git submodule update --init

# Configure with minimal dependencies
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${DEPLOY_DIR}/usr" \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_CTL=OFF \
    -DBUILD_TESTING=OFF \
    -DUSE_MYSQL=OFF \
    -DUSE_SQLITE3=OFF \
    -DUSE_POSTGRESQL=OFF \
    -DUSE_REDIS=OFF \
    -DUSE_BROTLI=OFF \
    -DUSE_SPDLOG=OFF \
    -DUSE_YAML_CPP=OFF

# Build and install
cmake --build build -j$(nproc)
cmake --install build

# Cleanup
rm -rf /tmp/drogon

echo "✓ Drogon ${DROGON_VERSION} installed"
