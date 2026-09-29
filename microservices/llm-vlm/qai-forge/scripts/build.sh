#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build.sh
#
# Main orchestrator script for building qai-forge and its dependencies.
# This script coordinates dependency builds and qai-forge compilation.
#
# Usage:
#   ./qai-forge/scripts/build.sh [OPTIONS]
#
# Options:
#   --install-prefix PATH       Installation prefix (default: /usr/local)
#   --deploy-dir PATH           Deployment directory (default: /build/deploy)
#   --jobs N                    Number of parallel jobs (default: nproc)
#   --enable-litert             Build LiteRT v2.1.5
#   --enable-litert-lm          Build LiteRT-LM v0.14.0
#   --enable-llamacpp           Build llama.cpp (plain variant)
#   --enable-llamacpp-hexagon   Build llama.cpp with Hexagon NPU
#   --skip-fastrpc              Skip fastrpc build (use platform package)
#   --skip-qairt                Skip QAIRT SDK fetch (use platform package)
#   --skip-deps                 Skip all dependency builds
#   --debug                     Enable debug output (set -x)
#   --help                      Show this help message
#
# Environment Variables:
#   All env vars from dependency scripts are supported
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QAI_FORGE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

INSTALL_PREFIX="${INSTALL_PREFIX:-/usr/local}"
DEPLOY_DIR="${DEPLOY_DIR:-/build/deploy}"
JOBS="${JOBS:-$(nproc)}"

BUILD_LITERT=false
BUILD_LITERT_LM=false
BUILD_LLAMACPP=false
BUILD_LLAMACPP_HEXAGON=false
SKIP_FASTRPC=false
SKIP_QAIRT=false
SKIP_DEPS=false
DEBUG=false

# nlohmann/json / stb / minja — pre-fetched (not via CMake FetchContent) so
# that the subsequent `cmake -S` configure step below never performs any
# network/git activity. Pinned commits match qai-forge/CMakeLists.txt's
# requirements.
NLOHMANN_JSON_SRC_DIR="${NLOHMANN_JSON_SRC_DIR:-/opt/third_party/nlohmann_json}"
NLOHMANN_JSON_GIT_REF="${NLOHMANN_JSON_GIT_REF:-9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03}"
STB_SRC_DIR="${STB_SRC_DIR:-/opt/third_party/stb}"
STB_GIT_REF="${STB_GIT_REF:-2c980bb59875b0d32144a71867fbdebb2f77cd20}"
MINJA_SRC_DIR="${MINJA_SRC_DIR:-/opt/third_party/minja}"
MINJA_GIT_REF="${MINJA_GIT_REF:-021c2293c187789ef13d56c6cfd89c9b134fd80f}"

# ─────────────────────────────────────────────────────────────────────────────
# Parse command line arguments
# ─────────────────────────────────────────────────────────────────────────────
show_help() {
    sed -n '/^# Usage:/,/^# Environment Variables:/p' "$0" | sed 's/^# //; s/^#//'
    exit 0
}

while [ $# -gt 0 ]; do
    case $1 in
        --install-prefix)
            INSTALL_PREFIX="$2"
            shift 2
            ;;
        --deploy-dir)
            DEPLOY_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --enable-litert)
            BUILD_LITERT=true
            shift
            ;;
        --enable-litert-lm)
            BUILD_LITERT_LM=true
            shift
            ;;
        --enable-llamacpp)
            BUILD_LLAMACPP=true
            shift
            ;;
        --enable-llamacpp-hexagon)
            BUILD_LLAMACPP_HEXAGON=true
            shift
            ;;
        --skip-fastrpc)
            SKIP_FASTRPC=true
            shift
            ;;
        --skip-qairt)
            SKIP_QAIRT=true
            shift
            ;;
        --skip-deps)
            SKIP_DEPS=true
            shift
            ;;
        --debug)
            DEBUG=true
            shift
            ;;
        --help)
            show_help
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Enable debug mode if requested
if [ "$DEBUG" = "true" ]; then
    set -x
    export DEBUG=1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Export environment variables for dependency scripts
# ─────────────────────────────────────────────────────────────────────────────
export DEPLOY_DIR
export SKIP_FASTRPC
export SKIP_QAIRT

echo "=== qai-forge Build Orchestrator ==="
echo "Install prefix: ${INSTALL_PREFIX}"
echo "Deploy directory: ${DEPLOY_DIR}"
echo "Parallel jobs: ${JOBS}"
echo "Build LiteRT: ${BUILD_LITERT}"
echo "Build LiteRT-LM: ${BUILD_LITERT_LM}"
echo "Build llama.cpp: ${BUILD_LLAMACPP}"
echo "Build llama.cpp+Hexagon: ${BUILD_LLAMACPP_HEXAGON}"
echo "Skip dependencies: ${SKIP_DEPS}"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Build dependencies
# ─────────────────────────────────────────────────────────────────────────────
if [ "$SKIP_DEPS" = "false" ]; then
    echo "=== Building dependencies ==="

    # Always build fastrpc (unless skipped)
    if [ "$SKIP_FASTRPC" = "false" ]; then
        echo "Building fastrpc..."
        sh "${SCRIPT_DIR}/deps/build-fastrpc.sh"
    fi

    # Always fetch QAIRT SDK (unless skipped)
    if [ "$SKIP_QAIRT" = "false" ]; then
        echo "Fetching QAIRT SDK..."
        sh "${SCRIPT_DIR}/deps/fetch-qairt-sdk.sh"
    fi

    # Always pre-fetch nlohmann/json — avoids any network activity during
    # the `cmake -S` configure step below (qai-forge/CMakeLists.txt requires
    # NLOHMANN_JSON_SOURCE_DIR unconditionally).
    if [ -f "${NLOHMANN_JSON_SRC_DIR}/CMakeLists.txt" ]; then
        echo "nlohmann/json already present at ${NLOHMANN_JSON_SRC_DIR} (pinned ${NLOHMANN_JSON_GIT_REF}); skipping fetch."
    else
        echo "Fetching nlohmann/json (pinned ${NLOHMANN_JSON_GIT_REF})..."
        git clone https://github.com/nlohmann/json.git "${NLOHMANN_JSON_SRC_DIR}"
        (cd "${NLOHMANN_JSON_SRC_DIR}" && git checkout "${NLOHMANN_JSON_GIT_REF}")
    fi

    # Always pre-fetch stb (header-only image decoder) — avoids any network
    # activity during the `cmake -S` configure step below.
    if [ -f "${STB_SRC_DIR}/stb_image.h" ]; then
        echo "stb already present at ${STB_SRC_DIR} (pinned ${STB_GIT_REF}); skipping fetch."
    else
        echo "Fetching stb (pinned ${STB_GIT_REF})..."
        git clone https://github.com/nothings/stb.git "${STB_SRC_DIR}"
        (cd "${STB_SRC_DIR}" && git checkout "${STB_GIT_REF}")
    fi

    # Build LiteRT if requested
    if [ "$BUILD_LITERT" = "true" ]; then
        echo "Building LiteRT..."
        sh "${SCRIPT_DIR}/deps/build-litert.sh"
    fi

    # Build LiteRT-LM if requested
    if [ "$BUILD_LITERT_LM" = "true" ]; then
        # Pre-fetch minja (Jinja2 subset used by LiteRTLMOrchestrator) —
        # avoids any network activity during the `cmake -S` configure step
        # below, since QAI_FORGE_BUILD_LITERT_LM=ON requires MINJA_SOURCE_DIR.
        if [ -f "${MINJA_SRC_DIR}/CMakeLists.txt" ]; then
            echo "minja already present at ${MINJA_SRC_DIR} (pinned ${MINJA_GIT_REF}); skipping fetch."
        else
            echo "Fetching minja (pinned ${MINJA_GIT_REF})..."
            git clone https://github.com/google/minja.git "${MINJA_SRC_DIR}"
            (cd "${MINJA_SRC_DIR}" && git checkout "${MINJA_GIT_REF}")
        fi

        echo "Building LiteRT-LM..."
        sh "${SCRIPT_DIR}/deps/build-litert-lm-standalone.sh"

        # Build Rust wrappers after LiteRT-LM
        echo "Building Rust wrappers..."
        sh "${SCRIPT_DIR}/deps/build-rust-wrappers.sh"
    fi

    # Build llama.cpp (plain variant)
    if [ "$BUILD_LLAMACPP" = "true" ]; then
        echo "Building llama.cpp..."
        sh "${SCRIPT_DIR}/deps/build-llamacpp.sh"
    fi

    # Build llama.cpp with Hexagon NPU
    if [ "$BUILD_LLAMACPP_HEXAGON" = "true" ]; then
        echo "Building llama.cpp with Hexagon NPU..."
        sh "${SCRIPT_DIR}/deps/build-llamacpp-hexagon.sh"
    fi

    echo "✓ Dependencies built successfully"
    echo ""
else
    echo "=== Skipping dependency builds (--skip-deps) ==="
    echo ""
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build qai-forge
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Building qai-forge ==="

# Create build directory
mkdir -p "${QAI_FORGE_DIR}/build"

# Configure with CMake
echo "Configuring qai-forge..."
cmake -S "${QAI_FORGE_DIR}" -B "${QAI_FORGE_DIR}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${DEPLOY_DIR}/usr" \
    -DGENIE_INCLUDE_PATH=/usr/include \
    -DGENIE_LIB_PATH=/usr/lib \
    -DNLOHMANN_JSON_SOURCE_DIR="${NLOHMANN_JSON_SRC_DIR}" \
    -DSTB_SOURCE_DIR="${STB_SRC_DIR}" \
    -DMINJA_SOURCE_DIR="${MINJA_SRC_DIR}" \
    -DQAI_FORGE_BUILD_LITERT="${BUILD_LITERT}" \
    -DQAI_FORGE_BUILD_LITERT_LM="${BUILD_LITERT_LM}" \
    -DLITERT_LM_INCLUDE_PATH=/usr/include \
    -DQAI_FORGE_BUILD_LLAMACPP="${BUILD_LLAMACPP}" \
    -DLLAMACPP_INCLUDE_PATH=/usr/include

# Build
echo "Building qai-forge..."
cmake --build "${QAI_FORGE_DIR}/build" -j"${JOBS}"

# Install
echo "Installing qai-forge to ${INSTALL_PREFIX}..."
cmake --install "${QAI_FORGE_DIR}/build" --prefix "${INSTALL_PREFIX}"

echo ""
echo "✓ qai-forge build complete"
echo ""
echo "Installation summary:"
echo "  Prefix: ${INSTALL_PREFIX}"
echo "  Libraries: ${INSTALL_PREFIX}/lib"
echo "  Headers: ${INSTALL_PREFIX}/include"
echo "  Binaries: ${INSTALL_PREFIX}/bin"
echo ""
echo "To use qai-forge, add to your environment:"
echo "  export LD_LIBRARY_PATH=${INSTALL_PREFIX}/lib:\$LD_LIBRARY_PATH"
echo "  export PATH=${INSTALL_PREFIX}/bin:\$PATH"
