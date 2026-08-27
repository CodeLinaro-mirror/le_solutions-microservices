#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-litert-lm-incremental.sh
#
# Builds LiteRT-LM v0.13.0 incrementally on top of already-built LiteRT.
# This matches the exact build logic from QAIServe/Dockerfile litert_lm_builder stage.
#
# Environment Variables (must be set to overwrite default values):
#   LITERT_SRC_DIR=/mnt/work/src/litert (from litert_builder stage)
#   LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
#   LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr
#   BUILD_LITERT_LM_CLI_TOOLS=false  Build the litert-lm / litert-lm-advanced
#                                     on-device debugging/benchmarking CLI
#                                     binaries.Set to "true" to build them.
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# ─────────────────────────────────────────────────────────────────────────────
# Defaults — used only when a variable isn't already set in the environment
# ─────────────────────────────────────────────────────────────────────────────
LITERT_SRC_DIR="${LITERT_SRC_DIR:-/mnt/work/src/litert}"
LITERT_LM_SRC_DIR="${LITERT_LM_SRC_DIR:-/mnt/work/src/litert-lm}"
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"
BUILD_LITERT_LM_CLI_TOOLS="${BUILD_LITERT_LM_CLI_TOOLS:-true}"

echo "=== Building LiteRT-LM v0.13.0 (incremental on top of LiteRT) ==="

# ─────────────────────────────────────────────────────────────────────────────
# Clone LiteRT-LM v0.13.0 with retry logic (POSIX-compatible)
# ─────────────────────────────────────────────────────────────────────────────
echo "Cloning LiteRT-LM v0.13.0..."
git lfs install

i=1
while [ "$i" -le 5 ]; do
    if git clone --branch v0.13.0 --depth 1 \
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
echo "Adding cc_shared_library target to c/BUILD..."
printf '\n# Shared library target for the LiteRT-LM C API.\n# Produces liblitert_lm_c_api.so that can be loaded at runtime.\n# Used by litert-lm-inference-worker in the QAIServe service.\ncc_shared_library(\n    name = "litert_lm_c_api",\n    exports_filter = ["//c:engine"],\n    deps = [":engine"],\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD

echo "Added cc_shared_library target to c/BUILD"
tail -20 c/BUILD

# ─────────────────────────────────────────────────────────────────────────────
# Build LiteRT-LM with Bazel (using already-built LiteRT via --override_repository)
# ─────────────────────────────────────────────────────────────────────────────
echo "Building LiteRT-LM with Bazel..."

# Build liblitert_lm_c_api.so as a self-contained shared library
bazel build \
    --config=linux \
    --config=linux_arm64 \
    --compilation_mode=opt \
    --override_repository=litert="${LITERT_SRC_DIR}" \
    --define=litert_enable_qnn=true \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --linkopt=-fuse-ld=lld \
    --http_timeout_scaling=10 \
    --experimental_repository_downloader_retries=10 \
    //c:litert_lm_c_api

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

echo "Searching for litert_lm_c_api .so in bazel outputs:"
find -L bazel-bin bazel-out \( -type f -o -type l \) \
    \( -name "liblitert_lm_c_api.so" -o -name "liblitert_lm_c_api.so.*" \
       -o -name "litert_lm_c_api.so" -o -name "litert_lm_c_api.so.*" \
       -o -name "libGemmaModelConstraintProvider.so" \) \
    2>/dev/null | tee /tmp/litert_lm_so_files.txt

echo "Copying found .so files:"
while IFS= read -r f; do
    base=$(basename "$f")
    if echo "$base" | grep -q "^lib"; then
        cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/" || true
    else
        cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/lib${base}" || true
    fi
done < /tmp/litert_lm_so_files.txt

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

echo "✓ LiteRT-LM v0.13.0 build complete (incremental)"
echo "  Libraries: ${LITERT_LM_DEPLOY_DIR}/lib"
echo "  Binaries: ${LITERT_LM_DEPLOY_DIR}/bin"
echo "  Headers: ${LITERT_LM_DEPLOY_DIR}/include"
