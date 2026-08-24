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
# Defaults — used only when a variable isn't already set in the environment
# ─────────────────────────────────────────────────────────────────────────────
LITERT_LM_VERSION="${LITERT_LM_VERSION:-0.14.0}"
LITERT_SRC_DIR="${LITERT_SRC_DIR:-/mnt/work/src/litert}"
LITERT_LM_SRC_DIR="${LITERT_LM_SRC_DIR:-/mnt/work/src/litert-lm}"
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"
BUILD_LITERT_LM_CLI_TOOLS="${BUILD_LITERT_LM_CLI_TOOLS:-true}"
QAIRT_LITERT_DIR="${QAIRT_LITERT_DIR:-/tmp/qairt-litert}"

echo "=== Building LiteRT-LM v${LITERT_LM_VERSION} (incremental on top of LiteRT) ==="

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT-LM v${LITERT_LM_VERSION} with retry logic (POSIX-compatible)
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT-LM v${LITERT_LM_VERSION}..."
git lfs install

i=1
while [ "$i" -le 5 ]; do
    if git clone --branch "v${LITERT_LM_VERSION}" --depth 1 \
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

# Ensure LFS artifacts are fully downloaded
git lfs pull

# ─────────────────────────────────────────────────────────────────────────────
# Add cc_shared_library target for the LiteRT-LM C API
# ─────────────────────────────────────────────────────────────────────────────
echo "Adding cc_binary target to c/BUILD..."
# Uses cc_binary+linkshared+linkstatic so all transitive deps (incl. Rust .a) are
# statically linked into a single self-contained .so with no Bazel internal NEEDED entries.
printf '\n# Self-contained shared library for the LiteRT-LM C API.\n# linkstatic=True pulls in all transitive Rust/C++ deps statically.\ncc_binary(\n    name = "liblitert_lm_c_api_full.so",\n    deps = [":engine"],\n    linkshared = True,\n    linkstatic = True,\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD

echo "Added cc_binary target to c/BUILD"
tail -10 c/BUILD

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT-LM with Bazel (using already-built LiteRT via --override_repository)
# ─────────────────────────────────────────────────────────────────────────────
echo "Building LiteRT-LM with Bazel..."

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

# Build the CLI tools separately with litert_link_capi_so=true so they
# dynamically load liblitert_runtime_c_api.so at runtime (avoids symbol conflicts)
#
# These are on-device debugging/benchmarking tools only.
# set BUILD_LITERT_LM_CLI_TOOLS=false to skip them.
if [ "$BUILD_LITERT_LM_CLI_TOOLS" = "true" ]; then
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
