#!/bin/sh
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

set -eu

# ============================================================================
# cherry-pick-system-libs.sh
#
# Cherry-picks required system libraries from the build environment into the
# deployment directory for the minimal runtime image.
# Includes additional libraries for gRPC and protobuf support.
# ============================================================================

EXPORT_DIR="${EXPORT_DIR:-/export}"

echo "=== Cherry-picking system libraries ==="

# Create target directory
mkdir -p "${EXPORT_DIR}/usr/lib"

# List of required system libraries (base + gRPC/protobuf + libcurl/libzip
# and their transitive TLS/protocol dependencies).
# NOTE: libssl/libcrypto are deliberately excluded — debian:trixie-slim (the
# qaiserve-runtime base image) already ships libssl.so.3/libcrypto.so.3 under
# /usr/lib/aarch64-linux-gnu/.
REQUIRED_LIBS="libyaml-0 libbsd libmd libjsoncpp libz libuuid libatomic libcurl libzip libnghttp2 libnghttp3 librtmp libidn2 libssh2 libpsl libgssapi_krb5 libkrb5 libk5crypto libcom_err libkrb5support libkeyutils "libldap*" "liblber*" libbrotlidec libbrotlicommon libzstd liblzma libbz2 libsasl2 libunistring libgnutls libhogweed libnettle libgmp libp11-kit libffi libtasn1 libgrpc++ libgrpc libgpr libaddress_sorting libprotobuf libre2 libcares libupb libgomp"

# Cherry-pick only the required system .so files (with symlinks preserved).
# These are libraries not already bundled via the builder deploy tree:
#   fastrpc deps:  libyaml-0-2, libbsd, libmd
#   Drogon deps:   libjsoncpp, libz, libuuid (libssl/libcrypto come from the
#                  runtime base image, see note above)
#   C++ atomics:   libatomic
#   gRPC/protobuf: libgrpc++, libgrpc, libprotobuf, and their transitive deps
#                  (abseil, re2, c-ares, gpr, upb, address_sorting)
#   llama.cpp:     libgomp (OpenMP runtime required by llama-server)
# trixie-slim already ships: libc6, libstdc++6, libgcc-s1, libm
#
# Uses find to locate .so files regardless of whether Debian placed them under
# /usr/lib/aarch64-linux-gnu/, /usr/lib/, or /lib/ — all three are searched.
for lib in $REQUIRED_LIBS; do
    find /usr/lib /usr/lib/aarch64-linux-gnu /lib -name "${lib}.so*" -exec cp -dp {} "${EXPORT_DIR}/usr/lib/" \; 2>/dev/null || true
done && \
    find /usr/lib /lib -name "libabsl_*.so*" -exec cp -dp {} /export/usr/lib/ \;
# Debian packages abseil as dozens of separately-named component libraries
# (libabsl_base.so.*, libabsl_strings.so.*, libabsl_synchronization.so.*, ...)
# rather than a single libabsl.so — the plain "${lib}.so*" glob above never
# matches any of them, so it is cherry-picked separately right above.

echo "✓ System libraries cherry-picked to ${EXPORT_DIR}/usr/lib"
