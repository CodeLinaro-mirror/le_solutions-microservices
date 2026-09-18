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
#   LLAMACPP_VERSION=2fb989b9e79bf4da8159855e24892c8f4c20300f
#                                                  llama.cpp version/branch/commit to build.
#                                                  Pinned (not "master") — commit d9f918d2d
#                                                  ("common: add json.h abstraction", 2026-08-22)
#                                                  changed common_chat_msgs_parse_oaicompat/
#                                                  common_chat_tools_parse_oaicompat to route
#                                                  through a new common_json wrapper that no
#                                                  longer accepts a raw nlohmann::ordered_json,
#                                                  breaking qai-forge/src/worker/llamacpp/main.cpp.
#                                                  This pin is 2fb989b9e — its parent commit,
#                                                  last known-good before that break.
#   HEXAGON_SDK_VERSION=6.6.0.0                   Hexagon SDK version
#   HEXAGON_TOOLS_VERSION=19.0.02                 Hexagon tools version
#   HEXAGON_ARCH=v73                              Hexagon architecture (v68, v73, v75)
#   LLAMACPP_INSTALL_DIR=/build/llamacpp-install  cmake install prefix (headers/libs
#                                                  qai-forge's own build looks for)
#   DEPLOY_DIR=/build/deploy                      Runtime staging dir copied into the
#                                                  final image (matches build-llamacpp.sh)
#   DEBUG=1                                       Enable debug output (set -x)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# Enable debug mode if requested
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
LLAMACPP_VERSION="${LLAMACPP_VERSION:-2fb989b9e79bf4da8159855e24892c8f4c20300f}"
HEXAGON_SDK_VERSION="${HEXAGON_SDK_VERSION:-6.6.0.0}"
HEXAGON_TOOLS_VERSION="${HEXAGON_TOOLS_VERSION:-19.0.02}"
HEXAGON_ARCH="${HEXAGON_ARCH:-v73}"
LLAMACPP_INSTALL_DIR="${LLAMACPP_INSTALL_DIR:-/build/llamacpp-install}"
DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"
LLAMACPP_BUILD_SERVER="${LLAMACPP_BUILD_SERVER:-ON}"

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
# Setup multi-arch for x86_64 libraries (needed for qaic compiler)
# ─────────────────────────────────────────────────────────────────────────────
echo "Setting up multi-arch support for x86_64..."
dpkg --add-architecture amd64

# Ubuntu and Debian codenames are not interchangeable (e.g. Debian's
# "trixie" is not an Ubuntu suite) — archive.ubuntu.com only has a matching
# suite when the base image actually IS Ubuntu. Detect that instead of
# blindly reusing this host's own codename for the Ubuntu archive URL.
DISTRO_ID="$(. /etc/os-release && echo "${ID}")"
if [ "${DISTRO_ID}" = "ubuntu" ]; then
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
else
    # Debian (and derivatives): deb.debian.org already mirrors amd64 packages
    # under the same suite name, no extra Ubuntu source needed — just letting
    # apt see the newly-added amd64 architecture against the existing sources
    # is sufficient.
    echo "Non-Ubuntu base (${DISTRO_ID}) detected — using existing Debian sources for amd64, skipping archive.ubuntu.com"
fi

apt-get update
apt-get install -y --no-install-recommends \
    libc6:amd64 \
    libstdc++6:amd64 \
    libgmp10:amd64 \
    qemu-user-static
apt-get clean && rm -rf /var/lib/apt/lists/*

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
echo "Cloning llama.cpp @ ${LLAMACPP_VERSION}..."
mkdir -p /build
cd /build
# `git clone --branch` only resolves branch/tag refs, not raw commit SHAs —
# LLAMACPP_VERSION is pinned to a commit (see comment above) to avoid
# floating on master and re-hitting upstream API breaks, so clone+fetch by
# ref instead. Works for branch/tag names too.
mkdir llama.cpp
cd llama.cpp
git init -q
git remote add origin https://github.com/ggerganov/llama.cpp.git
git fetch --depth 1 origin "${LLAMACPP_VERSION}"
git checkout -q FETCH_HEAD

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
    -DLLAMA_BUILD_SERVER="${LLAMACPP_BUILD_SERVER}" \
    -DLLAMA_BUILD_COMMON=ON \
    -DHEXAGON_SDK_ROOT="${HEXAGON_SDK_ROOT}" \
    -DHEXAGON_TOOLS_ROOT="${HEXAGON_TOOLS_ROOT}" \
    -DHEXAGON_CMAKE_ROOT="${HEXAGON_CMAKE_ROOT}" \
    -DHEXAGON_ARCH="${HEXAGON_ARCH}" \
    -DPREBUILT_LIB_DIR=linux_aarch64 \
    -DCMAKE_INSTALL_PREFIX="${LLAMACPP_INSTALL_DIR}" \
    -GNinja

cmake --build build -j4

# ─────────────────────────────────────────────────────────────────────────────
# Verify build artifacts
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Verifying build artifacts ==="
find build -name "*.so" -type f
if [ "${LLAMACPP_BUILD_SERVER}" = "ON" ]; then
    find build -name "llama-server" -type f
fi

# ─────────────────────────────────────────────────────────────────────────────
# Install artifacts directly to the deployment directory
# ─────────────────────────────────────────────────────────────────────────────
echo "Installing llama.cpp artifacts to ${LLAMACPP_INSTALL_DIR}..."
mkdir -p "${LLAMACPP_INSTALL_DIR}/lib/rfsa/adsp" \
         "${LLAMACPP_INSTALL_DIR}/include/ggml/include"

cmake --install build

echo "=== Verifying installation ==="
ls -la "${LLAMACPP_INSTALL_DIR}/lib/"
ls -la "${LLAMACPP_INSTALL_DIR}/bin/"

# Manual copy if cmake install didn't work
if [ -z "$(ls -A "${LLAMACPP_INSTALL_DIR}/lib"/*.so 2>/dev/null)" ]; then
    echo "WARNING: Libraries not installed, copying manually..."
    find build -name "*.so" -type f -exec cp -v {} "${LLAMACPP_INSTALL_DIR}/lib/" \;
fi

if [ "${LLAMACPP_BUILD_SERVER}" = "ON" ] && [ ! -f "${LLAMACPP_INSTALL_DIR}/bin/llama-server" ]; then
    echo "WARNING: llama-server not installed, copying manually..."
    find build -name "llama-server" -type f -exec cp -v {} "${LLAMACPP_INSTALL_DIR}/bin/" \;
fi

# ─────────────────────────────────────────────────────────────────────────────
# Reorganize Hexagon DSP blobs and copy headers
# ─────────────────────────────────────────────────────────────────────────────
echo "Staging DSP blobs and headers to ${LLAMACPP_INSTALL_DIR}..."

# Copy Hexagon DSP blobs (also needed under rfsa/adsp for the DSP loader,
# in addition to their install location in usr/lib)
find "${LLAMACPP_INSTALL_DIR}/lib" -maxdepth 1 -name 'libggml-htp-v*.so' \
    -exec cp {} "${LLAMACPP_INSTALL_DIR}/lib/rfsa/adsp/" \; 2>/dev/null || true

# Copy headers
cp -r /build/llama.cpp/ggml/include/* "${LLAMACPP_INSTALL_DIR}/include/ggml/include/" 2>/dev/null || true
cp -r /build/llama.cpp/include/* "${LLAMACPP_INSTALL_DIR}/include/" 2>/dev/null || true
# common/*.h (and its jinja/ subdirectory, which common/chat.h pulls in via
# quoted same-directory includes) — needed by llamacpp-inference-worker for
# common_chat_templates_apply()/sampler-chain construction. These are plain
# source headers, not CMake-installed PUBLIC_HEADERs, so stage them by hand.
mkdir -p "${LLAMACPP_INSTALL_DIR}/include/common"
cp -r /build/llama.cpp/common/*.h "${LLAMACPP_INSTALL_DIR}/include/common/" 2>/dev/null || true
if [ -d /build/llama.cpp/common/jinja ]; then
    cp -r /build/llama.cpp/common/jinja "${LLAMACPP_INSTALL_DIR}/include/common/"
fi

# mtmd.h/mtmd-helper.h (multimodal/vision support, --mmproj) — the mtmd
# library itself is already built by default (tools/mtmd is added
# unconditionally whenever LLAMA_BUILD_COMMON+LLAMA_BUILD_TOOLS are ON, both
# set above) and its .so is already swept up by the "*.so*" staging loop
# below, but these plain source headers live under tools/mtmd/ and aren't
# copied by the directory copies above — stage them by hand like common/*.h.
cp /build/llama.cpp/tools/mtmd/mtmd.h /build/llama.cpp/tools/mtmd/mtmd-helper.h \
    "${LLAMACPP_INSTALL_DIR}/include/" 2>/dev/null || true

# nlohmann/json.hpp — must be llama.cpp's OWN vendored copy (vendor/nlohmann),
# not the system nlohmann-json3-dev package qai-forge's own CMakeLists.txt
# pulls in via find_package(nlohmann_json). nlohmann/json encodes its release
# version into an ABI-tagged inline namespace (nlohmann::json_abi_vMAJOR_MINOR_PATCH),
# so common_chat_msgs_parse_oaicompat(nlohmann::ordered_json const&) mangles
# differently depending on which json.hpp compiled it. libllama-common.so is
# built against llama.cpp's vendored version (currently 3.12.0 ->
# json_abi_v3_12_0); if llamacpp-inference-worker's main.cpp instead resolves
# <nlohmann/json.hpp> to the system package (e.g. Ubuntu's 3.11.3 ->
# json_abi_v3_11_3), the two sides produce differently-mangled symbols for the
# "same" function, and linking fails with "undefined reference to
# common_chat_msgs_parse_oaicompat(...)" even though the function exists in
# libllama-common.so under the other namespace. Staging it here — ahead of
# LLAMACPP_INCLUDE_PATH's ggml/include entries in qai-forge's own
# target_include_directories() — makes #include <nlohmann/json.hpp> resolve
# to this copy first for any TU compiled against LLAMACPP_INCLUDE_PATH.
mkdir -p "${LLAMACPP_INSTALL_DIR}/include/nlohmann"
cp -v /build/llama.cpp/vendor/nlohmann/json.hpp /build/llama.cpp/vendor/nlohmann/json_fwd.hpp \
    "${LLAMACPP_INSTALL_DIR}/include/nlohmann/"

# Strip binaries and non-DSP shared libraries (stripping the Hexagon HTP
# blobs would corrupt them — they're compiled for the DSP core, not ARM64,
# same rationale the QAIServe Dockerfile's own strip step already documents).
find "${LLAMACPP_INSTALL_DIR}/bin" -type f -exec strip --strip-unneeded {} \; 2>/dev/null || true
find "${LLAMACPP_INSTALL_DIR}/lib" -maxdepth 1 -type f -name '*.so*' ! -name 'libggml-htp-v*.so' \
    -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage runtime artifacts into the final image's deploy tree
# ─────────────────────────────────────────────────────────────────────────────
# ${LLAMACPP_INSTALL_DIR} is only a build-time cmake prefix (headers +
# libs for qai-forge to link against) — it is not copied into the runtime
# image by itself. Mirror build-llamacpp.sh's convention and stage the
# actual .so/binaries the runtime needs into ${DEPLOY_DIR}, same as every
# other backend already does. The HTP DSP blobs go to rfsa/adsp only (never
# the flat lib dir) so the Dockerfile's later blanket `strip` over
# ${DEPLOY_DIR}/usr/lib (maxdepth 1) can't reach and corrupt them.
echo "Staging runtime artifacts to ${DEPLOY_DIR}..."
mkdir -p "${DEPLOY_DIR}/usr/bin" "${DEPLOY_DIR}/usr/lib/rfsa/adsp"
find "${LLAMACPP_INSTALL_DIR}/lib" -maxdepth 1 -name '*.so*' ! -name 'libggml-htp-v*.so' \
    -exec cp -Pv {} "${DEPLOY_DIR}/usr/lib/" \;
find "${LLAMACPP_INSTALL_DIR}/lib" -maxdepth 1 -name 'libggml-htp-v*.so' \
    -exec cp -v {} "${DEPLOY_DIR}/usr/lib/rfsa/adsp/" \;
if [ "${LLAMACPP_BUILD_SERVER}" = "ON" ] && [ -f "${LLAMACPP_INSTALL_DIR}/bin/llama-server" ]; then
    cp -v "${LLAMACPP_INSTALL_DIR}/bin/llama-server" "${DEPLOY_DIR}/usr/bin/"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
echo "Cleaning up build artifacts..."
rm -rf /build/llama.cpp

# The Hexagon SDK and Tools (~1GB+ combined between the SDK zip contents and
# the Tools tarball) are only needed to cross-compile the HTP DSP backend
# above — nothing later in the Dockerfile references /opt/Hexagon_SDK or
# /opt/hexagon, so leaving them in place only inflates this RUN layer with
# dead weight.
echo "Removing Hexagon SDK/Tools (no longer needed post-build)..."
rm -rf /opt/Hexagon_SDK /opt/hexagon

# The amd64 multiarch libs + qemu-user-static were only needed to run the
# x86_64 qaic IDL compiler under emulation during the build above; drop them
# now rather than leaving them installed in the layer. apt-get clean/rm
# lists alone (already done earlier) only clear the download cache, not the
# packages themselves.
echo "Removing amd64 multiarch/qemu build-only packages..."
apt-get purge -y --auto-remove \
    qemu-user-static \
    'libc6:amd64' \
    'libstdc++6:amd64' \
    'libgmp10:amd64' 2>/dev/null || true
dpkg --remove-architecture amd64 2>/dev/null || true

echo "✓ llama.cpp with Hexagon NPU build complete"
echo "  Build-time headers/libs: ${LLAMACPP_INSTALL_DIR}"
echo "  Runtime binaries: ${DEPLOY_DIR}/usr/bin"
echo "  Runtime libraries: ${DEPLOY_DIR}/usr/lib"
echo "  DSP blobs: ${DEPLOY_DIR}/usr/lib/rfsa/adsp"
