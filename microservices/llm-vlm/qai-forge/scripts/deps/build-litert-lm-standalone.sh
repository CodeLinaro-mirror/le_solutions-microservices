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
# Detect whether this stage is already running natively on the target
# architecture (arm64). See build-litert.sh for the same detection and its
# rationale — when true, skip the cross-compile toolchain and Rust/Bazel
# platform flags entirely, building natively instead.
# ─────────────────────────────────────────────────────────────────────────────
BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
LITERT_LM_VERSION="${LITERT_LM_VERSION:-v0.14.0}"
QAIRT_VERSION="${QAIRT_VERSION:-2.45.40.260406}"
LITERT_LM_SRC_DIR="${LITERT_LM_SRC_DIR:-/mnt/work/src/litert-lm}"
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"
QAIRT_DIR="${QAIRT_DIR:-/tmp/qairt}"
BAZELISK_VERSION="${BAZELISK_VERSION:-v1.28.1}"
# Bazel action concurrency. This build cross-compiles natively on the x86_64
# build host (see Dockerfile) rather than running an aarch64 Bazel under QEMU,
# so this can safely be raised to $(nproc).
BAZEL_JOBS="${BAZEL_JOBS:-4}"
export BAZEL_JOBS

echo "=== Building LiteRT-LM ${LITERT_LM_VERSION} ==="

# ─────────────────────────────────────────────────────────────────────────────
# Install Bazelisk
#
# x86_64 build — this script now runs natively on the x86_64 build host and
# cross-compiles to AArch64 (see Dockerfile), so an x86_64 Bazelisk drives the
# build. On a native arm64 host, download the arm64 Bazelisk instead.
# ─────────────────────────────────────────────────────────────────────────────
echo "Installing Bazelisk ${BAZELISK_VERSION}..."
if [ "${BUILD_ARCH}" = "arm64" ]; then
    BAZELISK_ASSET="bazelisk-linux-arm64"
else
    BAZELISK_ASSET="bazelisk-linux-amd64"
fi
wget -t 3 -T 120 --no-verbose \
    -O /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/${BAZELISK_ASSET}"
chmod +x /usr/local/bin/bazel

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT-LM
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT-LM ${LITERT_LM_VERSION}..."
git lfs install

i=1
while [ "$i" -le 8 ]; do
    # GIT_LFS_SKIP_SMUDGE keeps the clone itself fast and reliable — without it,
    # checkout inlines an LFS content fetch per pointer file and inherits
    # git-lfs's own ~30s activity timeout, so a single flaky LFS object aborts
    # the whole 976-file checkout ("Clone succeeded, but checkout failed") and
    # forces a full re-clone on retry. The actual LFS content is fetched by the
    # dedicated, separately-retried `git lfs pull` below instead.
    # timeout guards against a stalled connection that never errors out on its
    # own (observed on some networks: the clone hangs indefinitely at 0 bytes/s
    # instead of failing), which would otherwise bypass this retry loop entirely.
    if GIT_LFS_SKIP_SMUDGE=1 timeout 300 git clone --branch "${LITERT_LM_VERSION}" --depth 1 \
        https://github.com/google-ai-edge/LiteRT-LM.git "${LITERT_LM_SRC_DIR}"; then
        break
    else
        echo "Clone failed or timed out, retrying in 15 seconds..."
        rm -rf "${LITERT_LM_SRC_DIR}"
        sleep 15
        i=$((i + 1))
    fi
done
[ -d "${LITERT_LM_SRC_DIR}/.git" ] || { echo "ERROR: LiteRT-LM clone failed after 8 attempts"; exit 1; }

cd "${LITERT_LM_SRC_DIR}"

# LFS pull retried with a timeout for the same reason as the clone above: LFS
# object downloads have stalled indefinitely on some networks rather than
# failing outright.
i=1
while [ "$i" -le 8 ]; do
    if timeout 600 git lfs pull; then
        break
    else
        echo "LFS pull failed or timed out, retrying in 15 seconds..."
        sleep 15
        i=$((i + 1))
    fi
done
[ "$i" -le 8 ] || { echo "ERROR: LFS pull failed after 8 attempts"; exit 1; }

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

if [ "${BUILD_ARCH}" != "arm64" ]; then
    # ─────────────────────────────────────────────────────────────────────────
    # Register a platform-aware cc_toolchain for cross-compilation
    #
    # Same rationale as LiteRT's toolchains/BUILD (build-litert.sh) — LiteRT-LM's
    # local_config_embedded_arm toolchain isn't registered for Bazel's
    # platform-based toolchain resolution, so --platforms=...:linux_aarch64
    # fails without this. Only needed on the cross-compile path.
    # ─────────────────────────────────────────────────────────────────────────
    mkdir -p toolchains
    cp /tmp/litert-cross-compile-toolchains-BUILD toolchains/BUILD

    # ─────────────────────────────────────────────────────────────────────────
    # Add an aarch64-unknown-linux-gnu Rust target triple
    #
    # Same rationale as build-litert-lm-incremental.sh — LiteRT-LM depends on a
    # Rust component (runtime/components/rust: minijinja_template) and
    # rules_rust's rust_register_toolchains() call in WORKSPACE doesn't register
    # an aarch64-linux cross-compile target by default. Only needed on the
    # cross-compile path.
    # ─────────────────────────────────────────────────────────────────────────
    sed -i 's/"x86_64-apple-darwin",/"x86_64-apple-darwin",\n        "aarch64-unknown-linux-gnu",/' WORKSPACE
fi

# ─────────────────────────────────────────────────────────────────────────────
# Patch Clang-only target attribute syntax for GCC compatibility
#
# Unlike build-litert.sh (which clones LiteRT itself and can patch it
# directly), this script pulls LiteRT in as a Bazel WORKSPACE external repo
# (@litert), so the source tree only exists once Bazel fetches it. `bazel
# fetch` materializes it under output_base/external/litert without running
# any build actions, so it can be patched before the real `bazel build`
# below hits the same GCC/Clang target-attribute mismatch documented in
# build-litert.sh (target("dotprod") is Clang syntax; the aarch64
# cross-compiler here is GCC, which requires target("+dotprod")). Only
# needed on the cross-compile path — a native arm64 build uses Clang, which
# already accepts the unprefixed syntax.
# ─────────────────────────────────────────────────────────────────────────────
if [ "${BUILD_ARCH}" != "arm64" ]; then
    echo "Fetching @litert external repo to patch Clang-only attribute syntax..."
    bazel fetch @litert//tflite/kernels/internal:optimized_4bit
    BAZEL_OUTPUT_BASE=$(bazel info output_base)
    sed -i 's/target("dotprod")/target("+dotprod")/g' \
        "${BAZEL_OUTPUT_BASE}/external/litert/tflite/kernels/internal/optimized/4bit/neon_fully_connected_aarch64_sdot.cc" \
        "${BAZEL_OUTPUT_BASE}/external/litert/tflite/kernels/internal/optimized/4bit/neon_fully_connected_arm32_sdot.cc"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT-LM with Bazel
# ─────────────────────────────────────────────────────────────────────────────
echo "Building LiteRT-LM with Bazel (--jobs=${BAZEL_JOBS})..."
export LITERT_QAIRT_SDK="${QAIRT_DIR}/"

if [ "${BUILD_ARCH}" = "arm64" ]; then
    # Native build — same invocation this script used before cross-compile
    # support was added.
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
else
    # Cross-compile flags (this stage now runs natively on x86_64, see Dockerfile):
    #   --config=elinux_aarch64
    #       LiteRT-LM's own Bazel-provided embedded-Linux aarch64 config (picks
    #       the ARM cross-compiler).
    #   --host_cpu / --host_crosstool_top
    #       Keep host-only tools (codegen binaries) compiling for x86_64.
    #   --extra_toolchains / --platforms
    #       Bazel 7 platform-based resolution — see
    #       litert-cross-compile-toolchains/BUILD for why this is needed
    #       alongside --crosstool_top.
    #   --host_action_env=CC/CXX, --repo_env=CC/BAZEL_COMPILER
    #       Force host-only compilation onto clang-15 (verified necessary for
    #       LiteRT-LM's absl dependency in build-litert-lm-incremental.sh; kept
    #       here for the same reason since this script builds the same sources).
    bazel build \
        --config=linux \
        --config=elinux_aarch64 \
        --jobs="${BAZEL_JOBS}" \
        --compilation_mode=opt \
        --host_cpu=k8 \
        --host_crosstool_top=@bazel_tools//tools/cpp:toolchain \
        --extra_toolchains=//toolchains:cc_toolchain_aarch64 \
        --platforms=@org_tensorflow//tensorflow/tools/toolchains/linux:linux_aarch64 \
        --host_action_env=CC=/usr/lib/llvm-15/bin/clang \
        --host_action_env=CXX=/usr/lib/llvm-15/bin/clang++ \
        --repo_env=CC=/usr/lib/llvm-15/bin/clang \
        --repo_env=BAZEL_COMPILER=/usr/lib/llvm-15/bin/clang \
        --cxxopt=-std=c++20 \
        --host_cxxopt=-std=c++20 \
        --define=litert_enable_qnn=true \
        --define=litert_link_capi_so=true \
        --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --cxxopt=-fpermissive \
        --http_timeout_scaling=10 \
        --experimental_repository_downloader_retries=10 \
        //c:litert_lm_c_api
fi

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
