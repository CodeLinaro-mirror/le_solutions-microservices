#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-litert-lm-standalone.sh
#
# Builds LiteRT-LM v0.14.0 with Bazel on AArch64.
# This is the "standalone" variant that includes QAIRT SDK download.
#
# Environment Variables:
#   LITERT_LM_VERSION=v0.14.0       LiteRT-LM version to build
#   QAIRT_VERSION=2.45.40.260406    QAIRT SDK version
#   LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr  Deployment directory
#   BAZELISK_VERSION=v1.28.1        Bazelisk version
#   DEBUG=1                         Enable debug output (set -x)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# Enable debug mode if requested
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
LITERT_LM_VERSION="${LITERT_LM_VERSION:-v0.14.0}"
QAIRT_VERSION="${QAIRT_VERSION:-2.45.40.260406}"
LITERT_LM_SRC_DIR="${LITERT_LM_SRC_DIR:-/mnt/work/src/litert-lm}"
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"
QAIRT_DIR="${QAIRT_DIR:-/tmp/qairt}"
BAZELISK_VERSION="${BAZELISK_VERSION:-v1.28.1}"

echo "=== Building LiteRT-LM ${LITERT_LM_VERSION} ==="

# ─────────────────────────────────────────────────────────────────────────────
# Install Bazelisk
# ─────────────────────────────────────────────────────────────────────────────
echo "Installing Bazelisk ${BAZELISK_VERSION}..."
wget -t 3 -T 120 --no-verbose \
    -O /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64"
chmod +x /usr/local/bin/bazel

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT-LM
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT-LM ${LITERT_LM_VERSION}..."
git lfs install

i=1
while [ "$i" -le 5 ]; do
    if git clone --branch "${LITERT_LM_VERSION}" --depth 1 \
        https://github.com/google-ai-edge/LiteRT-LM.git "${LITERT_LM_SRC_DIR}"; then
        break
    else
        echo "Clone failed, retrying in 15 seconds..."
        rm -rf "${LITERT_LM_SRC_DIR}"
        sleep 15
        i=$((i + 1))
    fi
done

cd "${LITERT_LM_SRC_DIR}"
git lfs pull

# ─────────────────────────────────────────────────────────────────────────────
# Download QAIRT SDK
# ─────────────────────────────────────────────────────────────────────────────
echo "Downloading QAIRT SDK ${QAIRT_VERSION}..."
mkdir -p "${QAIRT_DIR}"
wget -t 3 -T 120 --no-verbose \
    -P "${QAIRT_DIR}" \
    "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip"

cd "${QAIRT_DIR}"
unzip "v${QAIRT_VERSION}.zip"
rm -f "v${QAIRT_VERSION}.zip"

# Create symlink for version compatibility
ln -sf "${QAIRT_DIR}/qairt/${QAIRT_VERSION}" \
       "${QAIRT_DIR}/qairt/2.44.0.260225"

echo "QAIRT ${QAIRT_VERSION} ready; symlink 2.44.0.260225 → ${QAIRT_VERSION}"

# ─────────────────────────────────────────────────────────────────────────────
# Add cc_shared_library target to LiteRT-LM
# ─────────────────────────────────────────────────────────────────────────────
cd "${LITERT_LM_SRC_DIR}"

echo "Adding cc_shared_library target to c/BUILD..."
printf '\n# Shared library target for the LiteRT-LM C API.\ncc_shared_library(\n    name = "litert_lm_c_api",\n    exports_filter = ["//c:engine"],\n    deps = [":engine"],\n    user_link_flags = ["-Wl,--export-dynamic"],\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT-LM with Bazel
# ─────────────────────────────────────────────────────────────────────────────
echo "Building LiteRT-LM with Bazel..."
export LITERT_QAIRT_SDK="${QAIRT_DIR}/"

bazel build \
    --config=linux \
    --config=linux_arm64 \
    --compilation_mode=opt \
    --action_env=CC=clang-15 \
    --action_env=CXX=clang++-15 \
    --cxxopt=-std=c++20 \
    --host_cxxopt=-std=c++20 \
    --define=litert_enable_qnn=true \
    --define=litert_link_capi_so=true \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --linkopt=-fuse-ld=lld \
    --http_timeout_scaling=10 \
    --experimental_repository_downloader_retries=10 \
    //c:litert_lm_c_api

# ─────────────────────────────────────────────────────────────────────────────
# Stage artifacts
# ─────────────────────────────────────────────────────────────────────────────
echo "Staging LiteRT-LM artifacts to ${LITERT_LM_DEPLOY_DIR}..."
mkdir -p "${LITERT_LM_DEPLOY_DIR}/lib" \
         "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c" \
         "${LITERT_LM_DEPLOY_DIR}/include/litert" \
         "${LITERT_LM_DEPLOY_DIR}/include/tflite"

# Copy shared libraries
find -L bazel-bin bazel-out \( -type f -o -type l \) \
    \( -name "liblitert_lm_c_api.so" -o -name "liblitert_lm_c_api.so.*" \
       -o -name "litert_lm_c_api.so" -o -name "litert_lm_c_api.so.*" \
       -o -name "libGemmaModelConstraintProvider.so" \
       -o -name "libllguidance*.so" -o -name "libllguidance*.so.*" \
       -o -name "liblitert_lm_llguidance*.so" -o -name "liblitert_lm_llguidance*.so.*" \) \
    2>/dev/null | while IFS= read -r f; do
        base=$(basename "$f")
        if echo "$base" | grep -q "^lib"; then
            cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/" || true
        else
            cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/lib${base}" || true
        fi
    done

# Copy transitive dependencies
if [ -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" ]; then
    readelf -d "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
        | grep NEEDED | sed 's/.*\[//;s/\]//' \
        | while IFS= read -r needed_lib; do
            if [ ! -f "${LITERT_LM_DEPLOY_DIR}/lib/$needed_lib" ]; then
                found=$(find -L bazel-bin bazel-out -name "$needed_lib" 2>/dev/null | head -1)
                if [ -n "$found" ]; then
                    cp -avL "$found" "${LITERT_LM_DEPLOY_DIR}/lib/"
                fi
            fi
        done
fi

# Copy headers
cp c/engine.h "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c/"

# Copy LiteRT headers from Bazel external
LITERT_EXTERNAL=$(find bazel-* -type d -path "*/external/litert" 2>/dev/null | head -1)
if [ -n "$LITERT_EXTERNAL" ] && [ -d "$LITERT_EXTERNAL" ]; then
    find "$LITERT_EXTERNAL" -type f \( -name "*.so" -o -name "*.so.*" \) \
        -exec cp -avL {} "${LITERT_LM_DEPLOY_DIR}/lib/" \; 2>/dev/null || true

    if [ -d "$LITERT_EXTERNAL/litert" ]; then
        cp -a "$LITERT_EXTERNAL/litert" "${LITERT_LM_DEPLOY_DIR}/include/" || true
    fi

    if [ -d "$LITERT_EXTERNAL/tflite" ]; then
        cp -a "$LITERT_EXTERNAL/tflite" "${LITERT_LM_DEPLOY_DIR}/include/" || true
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
echo "Cleaning up build artifacts..."
rm -rf "${QAIRT_DIR}"

echo "✓ LiteRT-LM ${LITERT_LM_VERSION} build complete"
echo "  Libraries: ${LITERT_LM_DEPLOY_DIR}/lib"
echo "  Headers: ${LITERT_LM_DEPLOY_DIR}/include"
