#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# cherry-pick-system-libs.sh
#
# Cherry-picks required system libraries from the build environment into the
# deployment directory for the minimal runtime image.
# ============================================================================

EXPORT_DIR="${EXPORT_DIR:-/export}"

echo "=== Cherry-picking system libraries ==="

# Create target directory
mkdir -p "${EXPORT_DIR}/usr/lib"

# List of required system libraries
REQUIRED_LIBS="libyaml-0 libbsd libmd libjsoncpp libssl libcrypto libz libuuid libatomic libgomp"

# Copy each library and its symlinks
for lib in $REQUIRED_LIBS; do
    find /usr/lib /lib -name "${lib}.so*" -exec cp -dp {} "${EXPORT_DIR}/usr/lib/" \; 2>/dev/null || true
done

echo "✓ System libraries cherry-picked to ${EXPORT_DIR}/usr/lib"
