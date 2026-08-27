#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-llamacpp-hexagon.sh
#
# Builds llama.cpp with Hexagon NPU backend support.
# This variant includes Hexagon SDK, DSP toolchain, and cross-compilation setup.
#
# Environment Variables:
#   LLAMACPP_VERSION=master                    llama.cpp version/branch to build
#   HEXAGON_SDK_VERSION=6.6.0.0                Hexagon SDK version
#   HEXAGON_TOOLS_VERSION=19.0.02              Hexagon tools version
#   HEXAGON_ARCH=v73                           Hexagon architecture (v68, v73, v75)
#   LLAMACPP_DEPLOY_DIR=/mnt/work/deploy/usr   Deployment directory
#   DEBUG=1                                    Enable debug output (set -x)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# Enable debug mode if requested
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
LLAMACPP_VERSION="${LLAMACPP_VERSION:-master}"
HEXAGON_SDK_VERSION="${HEXAGON_SDK_VERSION:-6.6.0.0}"
HEXAGON_TOOLS_VERSION="${HEXAGON_TOOLS_VERSION:-19.0.02}"
HEXAGON_ARCH="${HEXAGON_ARCH:-v73}"
LLAMACPP_DEPLOY_DIR="${LLAMACPP_DEPLOY_DIR:-/mnt/work/deploy/usr}"

echo "=== Building llama.cpp with Hexagon NPU support ==="

# ─────────────────────────────────────────────────────────────────────────────
# Build fastrpc (required for Hexagon DSP communication)
# ─────────────────────────────────────────────────────────────────────────────
echo "Building fastrpc..."
git clone --depth 1 https://github.com/quic/fastrpc.git /tmp/fastrpc
cd /tmp/fastrpc
autoreconf -is
CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
    ./configure --prefix=/usr --libdir=/usr/lib
make -j"$(nproc)"
make install
find /tmp/fastrpc -name 'remote.h' -exec cp {} /usr/include/ \;
rm -rf /tmp/fastrpc

# ─────────────────────────────────────────────────────────────────────────────
# Download Hexagon SDK
# ─────────────────────────────────────────────────────────────────────────────
echo "Downloading Hexagon SDK ${HEXAGON_SDK_VERSION}..."
wget --no-check-certificate \
    "https://softwarecenter.qualcomm.com/api/download/software/sdks/Hexagon_SDK/Linux/Debian/${HEXAGON_SDK_VERSION}/Hexagon_SDK_Linux.zip" \
    -O /tmp/hexagon_sdk.zip
unzip -q /tmp/hexagon_sdk.zip -d /opt
rm /tmp/hexagon_sdk.zip

# ─────────────────────────────────────────────────────────────────────────────
# Download Hexagon Tools
# ─────────────────────────────────────────────────────────────────────────────
echo "Downloading Hexagon Tools ${HEXAGON_TOOLS_VERSION}..."
wget --no-check-certificate \
    "https://softwarecenter.qualcomm.com/api/download/software/tools/Hexagon_open_access/Linux/ARM64/Debian/${HEXAGON_TOOLS_VERSION}/Hexagon_open_access.Core.${HEXAGON_TOOLS_VERSION}.Linux-ARM64.tar.gz" \
    -O /tmp/hexagon_tools.tar.gz
mkdir -p /opt/hexagon
tar -xzf /tmp/hexagon_tools.tar.gz -C /opt/hexagon/
rm /tmp/hexagon_tools.tar.gz

# ─────────────────────────────────────────────────────────────────────────────
# Setup Hexagon environment
# ─────────────────────────────────────────────────────────────────────────────
export HEXAGON_SDK_ROOT="/opt/Hexagon_SDK/${HEXAGON_SDK_VERSION}"
export HEXAGON_CMAKE_ROOT="${HEXAGON_SDK_ROOT}/build/cmake"
export HEXAGON_TOOLS_ROOT="/opt/hexagon"
export PATH="/opt/hexagon/Tools/bin:${PATH}"

# Copy remote.h to SDK locations
cp /usr/include/remote.h "${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/UbuntuARM_aarch64/"
cp /usr/include/remote.h "${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/android_aarch64/" 2>/dev/null || true

# Fix CMake string handling issue
sed -i 's/string(FIND \${PREBUILT_LIB_DIR} /string(FIND "${PREBUILT_LIB_DIR}" /g' \
    "${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake"

# ─────────────────────────────────────────────────────────────────────────────
# Setup multi-arch for x86_64 libraries (needed for qaic compiler)
# ─────────────────────────────────────────────────────────────────────────────
echo "Setting up multi-arch support for x86_64..."
dpkg --add-architecture amd64
# Read the codename off this host instead of hardcoding one.
UBUNTU_CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")"
echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${UBUNTU_CODENAME} main restricted universe multiverse" > /etc/apt/sources.list.d/amd64.list
echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${UBUNTU_CODENAME}-updates main restricted universe multiverse" >> /etc/apt/sources.list.d/amd64.list
echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu ${UBUNTU_CODENAME}-security main restricted universe multiverse" >> /etc/apt/sources.list.d/amd64.list

# Restrict the native repo to arm64 only, so `apt-get update` doesn't also
# try to fetch amd64 indices from it (ports.ubuntu.com never hosts amd64).
# Ubuntu 24.04+ ("noble") ships its default sources in the new DEB822 format
# at /etc/apt/sources.list.d/ubuntu.sources instead of the classic one-line
# /etc/apt/sources.list, so patch whichever file is actually populated.
if [ -s /etc/apt/sources.list.d/ubuntu.sources ]; then
    sed -i '/^Types:/i Architectures: arm64' /etc/apt/sources.list.d/ubuntu.sources
fi
if [ -s /etc/apt/sources.list ]; then
    sed -i 's/^deb /deb [arch=arm64] /' /etc/apt/sources.list
fi

apt-get update
apt-get install -y --no-install-recommends \
    libc6:amd64 \
    libstdc++6:amd64 \
    libgmp10:amd64
apt-get clean && rm -rf /var/lib/apt/lists/*

# ─────────────────────────────────────────────────────────────────────────────
# Setup qemu wrapper for qaic (x86_64 binary)
# ─────────────────────────────────────────────────────────────────────────────
echo "Setting up qemu wrapper for qaic..."
mkdir -p "${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin"
cat > "${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin/qaic" <<'EOF'
#!/bin/sh
exec qemu-x86_64-static /opt/Hexagon_SDK/6.6.0.0/ipc/fastrpc/qaic/Ubuntu/qaic "$@"
EOF
chmod +x "${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin/qaic"

# ─────────────────────────────────────────────────────────────────────────────
# Clone and build llama.cpp
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning llama.cpp..."
mkdir -p /build
cd /build
git clone --depth 1 https://github.com/ggerganov/llama.cpp.git
cd llama.cpp

# Patch GGML_SCHED_MAX_SPLIT_INPUTS for better performance
echo "Patching GGML_SCHED_MAX_SPLIT_INPUTS..."
sed -i 's/GGML_SCHED_MAX_SPLIT_INPUTS [0-9]*/GGML_SCHED_MAX_SPLIT_INPUTS 256/' ggml/src/ggml-backend.cpp

# ─────────────────────────────────────────────────────────────────────────────
# Build llama.cpp with Hexagon backend
# ─────────────────────────────────────────────────────────────────────────────
echo "Building llama.cpp with Hexagon backend..."
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGGML_HEXAGON=ON \
    -DGGML_NATIVE=OFF \
    -DGGML_BACKEND_DL=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=ON \
    -DHEXAGON_SDK_ROOT="${HEXAGON_SDK_ROOT}" \
    -DHEXAGON_TOOLS_ROOT="${HEXAGON_TOOLS_ROOT}" \
    -DHEXAGON_CMAKE_ROOT="${HEXAGON_CMAKE_ROOT}" \
    -DHEXAGON_ARCH="${HEXAGON_ARCH}" \
    -DPREBUILT_LIB_DIR=linux_aarch64 \
    -DCMAKE_INSTALL_PREFIX="${LLAMACPP_DEPLOY_DIR}" \
    -GNinja

cmake --build build -j4

# ─────────────────────────────────────────────────────────────────────────────
# Verify build artifacts
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Verifying build artifacts ==="
find build -name "*.so" -type f
find build -name "llama-server" -type f

# ─────────────────────────────────────────────────────────────────────────────
# Install artifacts directly to the deployment directory
# ─────────────────────────────────────────────────────────────────────────────
echo "Installing llama.cpp artifacts to ${LLAMACPP_DEPLOY_DIR}..."
mkdir -p "${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp" \
         "${LLAMACPP_DEPLOY_DIR}/include/ggml/include"

cmake --install build

echo "=== Verifying installation ==="
ls -la "${LLAMACPP_DEPLOY_DIR}/lib/"
ls -la "${LLAMACPP_DEPLOY_DIR}/bin/"

# Manual copy if cmake install didn't work
if [ -z "$(ls -A "${LLAMACPP_DEPLOY_DIR}/lib"/*.so 2>/dev/null)" ]; then
    echo "WARNING: Libraries not installed, copying manually..."
    find build -name "*.so" -type f -exec cp -v {} "${LLAMACPP_DEPLOY_DIR}/lib/" \;
fi

if [ ! -f "${LLAMACPP_DEPLOY_DIR}/bin/llama-server" ]; then
    echo "WARNING: llama-server not installed, copying manually..."
    find build -name "llama-server" -type f -exec cp -v {} "${LLAMACPP_DEPLOY_DIR}/bin/" \;
fi

# ─────────────────────────────────────────────────────────────────────────────
# Reorganize Hexagon DSP blobs and copy headers
# ─────────────────────────────────────────────────────────────────────────────
echo "Staging DSP blobs and headers to ${LLAMACPP_DEPLOY_DIR}..."

# Copy Hexagon DSP blobs (also needed under rfsa/adsp for the DSP loader,
# in addition to their install location in usr/lib)
find "${LLAMACPP_DEPLOY_DIR}/lib" -maxdepth 1 -name 'libggml-htp-v*.so' \
    -exec cp {} "${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp/" \; 2>/dev/null || true

# Copy headers
cp -r /build/llama.cpp/ggml/include/* "${LLAMACPP_DEPLOY_DIR}/include/ggml/include/" 2>/dev/null || true
cp -r /build/llama.cpp/include/* "${LLAMACPP_DEPLOY_DIR}/include/" 2>/dev/null || true

# Strip binaries
find "${LLAMACPP_DEPLOY_DIR}/bin" -type f -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
echo "Cleaning up build artifacts..."
rm -rf /build/llama.cpp

echo "✓ llama.cpp with Hexagon NPU build complete"
echo "  Binaries: ${LLAMACPP_DEPLOY_DIR}/bin"
echo "  Libraries: ${LLAMACPP_DEPLOY_DIR}/lib"
echo "  DSP blobs: ${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp"
echo "  Headers: ${LLAMACPP_DEPLOY_DIR}/include"
