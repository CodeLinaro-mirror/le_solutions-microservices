#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-litert-lm-incremental.sh
#
# Builds LiteRT-LM incrementally on top of already-built LiteRT.
# This matches the exact build logic from QAIServe/Dockerfile litert_lm_builder stage.
#
# Environment Variables (must be set to overwrite default values):
#   LITERT_LM_VERSION=0.14.0
#   LITERT_SRC_DIR=/mnt/work/src/litert (from litert_builder stage)
#   LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
#   LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr
#   BUILD_LITERT_LM_CLI_TOOLS=false  Build the litert-lm / litert-lm-advanced
#                                     on-device debugging/benchmarking CLI
#                                     binaries.Set to "true" to build them.
#   QAIRT_LITERT_DIR=/tmp/qairt-litert  QAIRT SDK dir staged by litert_builder
#                                         (build-litert.sh); removed at the end
#                                         of this script since litert_lm_builder
#                                         is the last stage in the chain to need it.
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# ─────────────────────────────────────────────────────────────────────────────
# Detect whether this stage is already running natively on the target
# architecture (arm64). See build-litert.sh for the same detection and its
# rationale — when true, skip the cross-compile toolchain and Rust/Bazel
# platform flags entirely, building natively instead.
# ─────────────────────────────────────────────────────────────────────────────
BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"

# ─────────────────────────────────────────────────────────────────────────────
# Defaults — used only when a variable isn't already set in the environment
# ─────────────────────────────────────────────────────────────────────────────
LITERT_LM_VERSION="${LITERT_LM_VERSION:-0.14.0}"
LITERT_SRC_DIR="${LITERT_SRC_DIR:-/mnt/work/src/litert}"
LITERT_LM_SRC_DIR="${LITERT_LM_SRC_DIR:-/mnt/work/src/litert-lm}"
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"
BUILD_LITERT_LM_CLI_TOOLS="${BUILD_LITERT_LM_CLI_TOOLS:-true}"
QAIRT_LITERT_DIR="${QAIRT_LITERT_DIR:-/tmp/qairt-litert}"
# Bazel action concurrency. This build cross-compiles natively on the x86_64
# build host (see Dockerfile — this stage is FROM litert_builder, which is
# now BUILDPLATFORM/x86_64) rather than running an aarch64 Bazel under QEMU,
# so this can safely be raised to $(nproc).
BAZEL_JOBS="${BAZEL_JOBS:-4}"
export BAZEL_JOBS

echo "=== Building LiteRT-LM v${LITERT_LM_VERSION} (incremental on top of LiteRT) ==="

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT-LM v${LITERT_LM_VERSION} with retry logic (POSIX-compatible)
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT-LM v${LITERT_LM_VERSION}..."
git lfs install

i=1
while [ "$i" -le 8 ]; do
    # GIT_LFS_SKIP_SMUDGE avoids inheriting git-lfs's ~30s per-pointer timeout
    # during checkout; LFS content is fetched separately below via `git lfs
    # pull`. `timeout` guards against connections that stall instead of erroring.
    if GIT_LFS_SKIP_SMUDGE=1 timeout 300 git clone --branch "v${LITERT_LM_VERSION}" --depth 1 \
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

# Ensure LFS artifacts are fully downloaded — retried with a timeout for the
# same reason as the clone above: LFS object downloads have stalled
# indefinitely on some networks rather than failing outright.
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
# Add cc_shared_library target for the LiteRT-LM C API
# ─────────────────────────────────────────────────────────────────────────────
echo "Adding cc_binary target to c/BUILD..."
# Uses cc_binary+linkshared+linkstatic so all transitive deps (incl. Rust .a) are
# statically linked into a single self-contained .so with no Bazel internal NEEDED entries.
printf '\n# Self-contained shared library for the LiteRT-LM C API.\n# linkstatic=True pulls in all transitive Rust/C++ deps statically.\ncc_binary(\n    name = "liblitert_lm_c_api_full.so",\n    deps = [":engine"],\n    linkshared = True,\n    linkstatic = True,\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD

echo "Added cc_binary target to c/BUILD"
tail -10 c/BUILD

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
    # LiteRT-LM depends on a Rust component (runtime/components/rust:
    # minijinja_template). rules_rust's rust_register_toolchains() call in
    # WORKSPACE only lists Android/iOS/macOS triples in extra_target_triples —
    # desktop platforms are registered by default, but only for the build host's
    # own architecture, not for cross-compiling to aarch64-linux. Without this,
    # toolchain resolution for the Rust target fails ("No matching toolchains
    # found for types @@rules_rust//rust:toolchain_type"). Only needed on the
    # cross-compile path — a native arm64 build already has aarch64-linux
    # registered as its own host triple.
    # ─────────────────────────────────────────────────────────────────────────
    sed -i 's/"x86_64-apple-darwin",/"x86_64-apple-darwin",\n        "aarch64-unknown-linux-gnu",/' WORKSPACE
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT-LM with Bazel (using already-built LiteRT via --override_repository)
# ─────────────────────────────────────────────────────────────────────────────
echo "Building LiteRT-LM with Bazel (--jobs=${BAZEL_JOBS})..."
if [ "${BUILD_ARCH}" = "arm64" ]; then
    # Native build — same invocation this script used before cross-compile
    # support was added.
    bazel build \
        --config=linux \
        --config=linux_arm64 \
        --compilation_mode=opt \
        --action_env=CC=clang-15 \
        --action_env=CXX=clang++-15 \
        --override_repository=litert="${LITERT_SRC_DIR}" \
        --define=litert_enable_qnn=true \
        --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --linkopt=-fuse-ld=lld \
        --http_timeout_scaling=10 \
        --experimental_repository_downloader_retries=10 \
        //c:liblitert_lm_c_api_full.so
else
    # Cross-compile flags (this stage now runs natively on x86_64, see Dockerfile):
    #   --config=elinux_aarch64: LiteRT-LM's embedded-Linux aarch64 config.
    #   --host_cpu/--host_crosstool_top: keep host-only tools on x86_64.
    #   --extra_toolchains/--platforms: Bazel 7 platform-based toolchain
    #     resolution (see litert-cross-compile-toolchains/BUILD).
    #   --host_action_env/--repo_env: force host-only compilation onto
    #     clang-15 (needed for C++20 std::source_location in absl).
    #   --cxxopt=-fpermissive: GCC rejects a Clang-legal Span() pattern in
    #     litert/cc/litert_buffer_ref.h; LiteRT-LM's .bazelrc lacks the
    #     -fpermissive LiteRT's own .bazelrc already carries for this.
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
        --override_repository=litert="${LITERT_SRC_DIR}" \
        --define=litert_enable_qnn=true \
        --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
        --cxxopt=-fpermissive \
        --http_timeout_scaling=10 \
        --experimental_repository_downloader_retries=10 \
        //c:liblitert_lm_c_api_full.so
fi

# Build the CLI tools separately with litert_link_capi_so=true so they
# dynamically load liblitert_runtime_c_api.so at runtime (avoids symbol conflicts)
#
# These are on-device debugging/benchmarking tools only.
# set BUILD_LITERT_LM_CLI_TOOLS=false to skip them.
if [ "$BUILD_LITERT_LM_CLI_TOOLS" = "true" ]; then
    if [ "${BUILD_ARCH}" = "arm64" ]; then
        bazel build \
            --config=linux \
            --config=linux_arm64 \
            --compilation_mode=opt \
            --action_env=CC=clang-15 \
            --action_env=CXX=clang++-15 \
            --override_repository=litert="${LITERT_SRC_DIR}" \
            --define=litert_enable_qnn=true \
            --define=litert_link_capi_so=true \
            --define=resolve_symbols_in_exec=false \
            --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
            --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
            --linkopt=-fuse-ld=lld \
            //runtime/engine:litert_lm_main \
            //runtime/engine:litert_lm_advanced_main
    else
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
            --override_repository=litert="${LITERT_SRC_DIR}" \
            --define=litert_enable_qnn=true \
            --define=litert_link_capi_so=true \
            --define=resolve_symbols_in_exec=false \
            --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
            --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
            --cxxopt=-fpermissive \
            //runtime/engine:litert_lm_main \
            //runtime/engine:litert_lm_advanced_main
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage LiteRT-LM shared library, C API headers, and CLI tool binaries
# ─────────────────────────────────────────────────────────────────────────────
echo "Staging LiteRT-LM artifacts to ${LITERT_LM_DEPLOY_DIR}..."
mkdir -p "${LITERT_LM_DEPLOY_DIR}/lib" \
         "${LITERT_LM_DEPLOY_DIR}/bin" \
         "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c"

# Copy the self-contained .so (linkstatic=True, all deps statically linked)
CC_BINARY_SO=$(find -L bazel-bin/c -maxdepth 1 -name "liblitert_lm_c_api_full.so" -type f 2>/dev/null | head -1)
if [ -z "$CC_BINARY_SO" ]; then
    echo "ERROR: liblitert_lm_c_api_full.so not found in bazel-bin/c"
    exit 1
fi
echo "Staging: $CC_BINARY_SO"
cp -avL "$CC_BINARY_SO" "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so"

# Copy additional runtime .so files that are dynamically loaded by LiteRT-LM
# (not statically linked due to plugin architecture)
find -L bazel-bin -name "libGemmaModelConstraintProvider.so" -type f 2>/dev/null | head -1 | while read f; do
    echo "Staging runtime plugin: $f"
    cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/"
done

# Copy headers
cp c/engine.h "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c/"

# Copy CLI binaries (or stub them if BUILD_LITERT_LM_CLI_TOOLS=false)
if [ "$BUILD_LITERT_LM_CLI_TOOLS" = "true" ]; then
    cp -avL bazel-bin/runtime/engine/litert_lm_main \
           "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm"
    cp -avL bazel-bin/runtime/engine/litert_lm_advanced_main \
           "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm-advanced"
else
    echo "Skipping litert-lm CLI tools build (BUILD_LITERT_LM_CLI_TOOLS=false); staging stubs instead"
    for tool in litert-lm litert-lm-advanced; do
        printf '#!/bin/sh\necho "%s was not built into this image (BUILD_LITERT_LM_CLI_TOOLS=false)."\necho "Rebuild with after setting the ENV var BUILD_LITERT_LM_CLI_TOOLS=true"\nexit 1\n' \
            "$tool" > "${LITERT_LM_DEPLOY_DIR}/bin/${tool}"
        chmod +x "${LITERT_LM_DEPLOY_DIR}/bin/${tool}"
    done
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build Rust cdylib wrappers (libllguidance.so) required by liblitert_lm_c_api.so
# Must run after Bazel so the Bazel cache contains the llguidance Rust source.
# ─────────────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "${SCRIPT_DIR}/build-rust-wrappers.sh" ]; then
    echo "Building Rust wrappers (libllguidance.so)..."
    chmod +x "${SCRIPT_DIR}/build-rust-wrappers.sh"
    LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR}" \
        sh "${SCRIPT_DIR}/build-rust-wrappers.sh" || \
        echo "WARNING: build-rust-wrappers.sh failed or skipped — llg_new_tokenizer may be missing"
else
    echo "WARNING: build-rust-wrappers.sh not found, skipping Rust wrappers"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Verify staged artifacts
# ─────────────────────────────────────────────────────────────────────────────
echo "Staged LiteRT-LM library:"
ls -lah "${LITERT_LM_DEPLOY_DIR}/lib/"
test -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so"
echo "✓ liblitert_lm_c_api.so staged successfully"

echo "Staged LiteRT-LM CLI tools:"
ls -lah "${LITERT_LM_DEPLOY_DIR}/bin/"
test -f "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm"
test -f "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm-advanced"
echo "✓ litert-lm CLI tools staged successfully"

echo "✓ LiteRT-LM v${LITERT_LM_VERSION} build complete (incremental)"
echo "  Libraries: ${LITERT_LM_DEPLOY_DIR}/lib"
echo "  Binaries: ${LITERT_LM_DEPLOY_DIR}/bin"
echo "  Headers: ${LITERT_LM_DEPLOY_DIR}/include"

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
#
# litert_lm_builder is the last stage in the litert_builder -> litert_lm_builder
# chain that needs the QAIRT SDK staged by build-litert.sh (via QAIRT_LITERT_DIR
# / LITERT_QAIRT_SDK), so it's safe to remove it here now that both bazel
# builds are done.
# ─────────────────────────────────────────────────────────────────────────────
echo "Cleaning up QAIRT SDK staging directory..."
rm -rf "${QAIRT_LITERT_DIR}"
