#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-litert.sh
#
# Builds LiteRT v2.1.5 with QNN dispatch and compiler plugin for QAIServe.
# This matches the exact build logic from QAIServe/Dockerfile litert_builder stage.
#
# Environment Variables (must be set to overwrite default values):
#   LITERT_SRC_DIR=/mnt/work/src/litert
#   LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
#   QAIRT_LITERT_VERSION=2.45.40.260406
#   QAIRT_LITERT_DIR=/tmp/qairt-litert
#   HERMETIC_PYTHON_VERSION=3.10
#   BAZELISK_VERSION=v1.28.1
#   PYTHON_BIN_PATH=/usr/bin/python3
#   PYTHON_LIB_PATH=/usr/lib/python3/dist-packages
#   TF_NEED_* (various TensorFlow config vars)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# ─────────────────────────────────────────────────────────────────────────────
# Defaults — used only when a variable isn't already set in the environment
# ─────────────────────────────────────────────────────────────────────────────
LITERT_SRC_DIR="${LITERT_SRC_DIR:-/mnt/work/src/litert}"
LITERT_DEPLOY_DIR="${LITERT_DEPLOY_DIR:-/mnt/work/deploy/usr}"
QAIRT_LITERT_VERSION="${QAIRT_LITERT_VERSION:-2.45.40.260406}"
QAIRT_LITERT_DIR="${QAIRT_LITERT_DIR:-/tmp/qairt-litert}"
HERMETIC_PYTHON_VERSION="${HERMETIC_PYTHON_VERSION:-3.10}"
PYTHON_BIN_PATH="${PYTHON_BIN_PATH:-/usr/bin/python3}"
PYTHON_LIB_PATH="${PYTHON_LIB_PATH:-/usr/lib/python3/dist-packages}"
TF_NEED_CUDA="${TF_NEED_CUDA:-0}"
TF_NEED_ROCM="${TF_NEED_ROCM:-0}"
TF_NEED_TENSORRT="${TF_NEED_TENSORRT:-0}"
TF_NEED_OPENCL_SYCL="${TF_NEED_OPENCL_SYCL:-0}"
TF_NEED_OPENCL="${TF_NEED_OPENCL:-0}"
TF_NEED_MPI="${TF_NEED_MPI:-0}"
TF_NEED_COMPUTECPP="${TF_NEED_COMPUTECPP:-0}"
TF_NEED_CLANG="${TF_NEED_CLANG:-1}"
CLANG_COMPILER_PATH="${CLANG_COMPILER_PATH:-/usr/bin/clang}"
TF_SET_ANDROID_WORKSPACE="${TF_SET_ANDROID_WORKSPACE:-0}"
TF_DOWNLOAD_CLANG="${TF_DOWNLOAD_CLANG:-0}"
TF_ENABLE_XLA="${TF_ENABLE_XLA:-0}"
export PYTHON_BIN_PATH PYTHON_LIB_PATH \
    TF_NEED_CUDA TF_NEED_ROCM TF_NEED_TENSORRT TF_NEED_OPENCL_SYCL TF_NEED_OPENCL \
    TF_NEED_MPI TF_NEED_COMPUTECPP TF_NEED_CLANG CLANG_COMPILER_PATH \
    TF_SET_ANDROID_WORKSPACE TF_DOWNLOAD_CLANG TF_ENABLE_XLA

echo "=== Building LiteRT v2.1.5 with QNN dispatch and compiler plugin ==="

# ─────────────────────────────────────────────────────────────────────────────
# Install Bazelisk
# ─────────────────────────────────────────────────────────────────────────────
BAZELISK_VERSION="${BAZELISK_VERSION:-v1.28.1}"
echo "Installing Bazelisk ${BAZELISK_VERSION}..."
wget -t 3 -T 120 --no-verbose \
    -O /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64"
chmod +x /usr/local/bin/bazel

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT v2.1.5 with retry logic (POSIX-compatible)
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT v2.1.5..."
git config --global http.postBuffer 1048576000
git config --global http.lowSpeedLimit 0
git config --global http.lowSpeedTime 999999
git config --global core.compression 0

i=1
while [ "$i" -le 5 ]; do
    if git clone --branch v2.1.5 --depth 1 https://github.com/google-ai-edge/LiteRT.git "${LITERT_SRC_DIR}"; then
        break
    else
        echo "Clone failed, retrying in 15 seconds..."
        rm -rf "${LITERT_SRC_DIR}"
        sleep 15
        i=$((i + 1))
    fi
done

cd "${LITERT_SRC_DIR}"

# Clone submodules with retry logic
i=1
while [ "$i" -le 5 ]; do
    if git submodule update --init --recursive --depth 1; then
        break
    else
        echo "Submodule update failed, retrying in 15 seconds..."
        sleep 15
        i=$((i + 1))
    fi
done

# Run configure if present
if [ -f configure ]; then
    yes n | ./configure
elif [ -f configure.py ]; then
    yes n | python3 configure.py
else
    echo "No configure script found; using env-based configuration"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Download QAIRT SDK
# ─────────────────────────────────────────────────────────────────────────────
echo "Downloading QAIRT SDK ${QAIRT_LITERT_VERSION}..."
mkdir -p "${QAIRT_LITERT_DIR}"
QAIRT_ZIP="${QAIRT_LITERT_DIR}/v${QAIRT_LITERT_VERSION}.zip"
QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_LITERT_VERSION}/v${QAIRT_LITERT_VERSION}.zip"
i=1
while [ "$i" -le 10 ]; do
    wget -c -t 1 -T 120 --no-verbose -O "${QAIRT_ZIP}" "${QAIRT_URL}" && break
    echo "Download attempt $i failed, retrying in 15 seconds..."
    sleep 15
    i=$((i + 1))
done
[ -f "${QAIRT_ZIP}" ] || { echo "ERROR: QAIRT SDK download failed after 10 attempts"; exit 1; }

cd "${QAIRT_LITERT_DIR}"
unzip "${QAIRT_ZIP}"
rm -f "${QAIRT_ZIP}"

# Create compatibility symlink
ln -sf "${QAIRT_LITERT_DIR}/qairt/${QAIRT_LITERT_VERSION}" \
       "${QAIRT_LITERT_DIR}/qairt/2.44.0.260225"

echo "QAIRT ${QAIRT_LITERT_VERSION} ready; symlink 2.44.0.260225 → ${QAIRT_LITERT_VERSION}"

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT with Bazel
# ─────────────────────────────────────────────────────────────────────────────
cd "${LITERT_SRC_DIR}"

mkdir -p /bazel-cache

echo "Building LiteRT with Bazel..."
bazel build --config=linux -c opt \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --copt=-DLITERT_ENABLE_NPU \
    --linkopt=-Wl,--whole-archive \
    --linkopt=-Wl,--no-whole-archive \
    //litert/c:litert_runtime_c_api_so \
    //litert/c:litert_tensor_buffer \
    //litert/runtime:tensor_buffer \
    //litert/vendors/qualcomm/dispatch:dispatch_api_so \
    //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so \
    //litert/tools:run_model \
    //litert/tools:run_model_simple \
    //litert/tools:benchmark_model

# ─────────────────────────────────────────────────────────────────────────────
# Stage LiteRT shared libraries, plugins, binaries, and public headers
# ─────────────────────────────────────────────────────────────────────────────
echo "Staging LiteRT artifacts to ${LITERT_DEPLOY_DIR}..."
mkdir -p "${LITERT_DEPLOY_DIR}/lib" "${LITERT_DEPLOY_DIR}/bin" "${LITERT_DEPLOY_DIR}/include"

bazel cquery \
    --config=linux -c opt \
    --repo_env=HERMETIC_PYTHON_VERSION="${HERMETIC_PYTHON_VERSION}" \
    --output=files \
    'set(//litert/c:litert_runtime_c_api_so //litert/c:litert_tensor_buffer //litert/runtime:tensor_buffer //litert/vendors/qualcomm/dispatch:dispatch_api_so //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so //litert/tools:run_model //litert/tools:run_model_simple //litert/tools:benchmark_model)' \
    > /tmp/litert_bazel_outputs.txt

echo "Configured LiteRT Bazel outputs:"
cat /tmp/litert_bazel_outputs.txt

while IFS= read -r output; do
    if [ -e "$output" ] || [ -L "$output" ]; then
        case "$output" in
            *.so|*.so.*|*.a) cp -avL "$output" "${LITERT_DEPLOY_DIR}/lib/" ;;
            */run_model|*/run_model_simple|*/benchmark_model) cp -avL "$output" "${LITERT_DEPLOY_DIR}/bin/" ;;
        esac
    fi
done < /tmp/litert_bazel_outputs.txt

# Additional search for binaries
find bazel-bin bazel-out \( -type f -o -type l \) \
    | grep -E '(/run_model|/run_model_simple|/benchmark_model)$' \
    | while IFS= read -r output; do cp -avL "$output" "${LITERT_DEPLOY_DIR}/bin/" || true; done

# Copy headers
cp -a litert "${LITERT_DEPLOY_DIR}/include/"
cp -a tflite "${LITERT_DEPLOY_DIR}/include/"

# Generate build_config.h from build_config.h.in
echo "Generating build_config.h from build_config.h.in (NPU enabled, GPU disabled)..."
mkdir -p "${LITERT_DEPLOY_DIR}/include/litert/build_common"
sed \
    -e 's/#cmakedefine01 LITERT_BUILD_CONFIG_DISABLE_GPU/#define LITERT_BUILD_CONFIG_DISABLE_GPU 1/' \
    -e 's/#cmakedefine01 LITERT_BUILD_CONFIG_DISABLE_NPU/#define LITERT_BUILD_CONFIG_DISABLE_NPU 0/' \
    litert/build_common/build_config.h.in \
    > "${LITERT_DEPLOY_DIR}/include/litert/build_common/build_config.h"

# ─────────────────────────────────────────────────────────────────────────────
# Verify staged artifacts
# ─────────────────────────────────────────────────────────────────────────────
echo "Staged LiteRT libraries and tools:"
ls -lah "${LITERT_DEPLOY_DIR}/lib"
ls -lah "${LITERT_DEPLOY_DIR}/bin"

# Verification checks
test "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f | wc -l)" -gt 0
test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*litert*.so' -o -iname '*litert*.so.*' -o -iname '*LiteRt*.so' -o -iname '*LiteRt*.so.*' -o -iname '*litert*.a' -o -iname '*LiteRt*.a' \) | head -1)"
test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*dispatch*.so' -o -iname '*dispatch*.so.*' -o -iname '*qnn*plugin*.so' -o -iname '*qnn*plugin*.so.*' -o -iname '*dispatch*.a' -o -iname '*qnn*plugin*.a' \) | head -1)"
test -f "${LITERT_DEPLOY_DIR}/bin/run_model"
test -f "${LITERT_DEPLOY_DIR}/bin/run_model_simple"
test -f "${LITERT_DEPLOY_DIR}/bin/benchmark_model"

echo "✓ LiteRT v2.1.5 build complete"
echo "  Libraries: ${LITERT_DEPLOY_DIR}/lib"
echo "  Binaries: ${LITERT_DEPLOY_DIR}/bin"
echo "  Headers: ${LITERT_DEPLOY_DIR}/include"
