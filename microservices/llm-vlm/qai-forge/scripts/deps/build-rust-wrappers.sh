#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause-Clear
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.

# ─────────────────────────────────────────────────────────────────────────────
# build-rust-wrappers.sh
#
# Builds Rust cdylib wrappers for llguidance and tokenizers_c.
# These wrappers are needed by LiteRT-LM for constrained generation.
#
# Prerequisites:
#   - LiteRT-LM must be built first (Bazel cache contains source dependencies)
#   - patchelf must be installed
#
# Environment Variables:
#   LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr  Deployment directory
#   DEBUG=1                                     Enable debug output (set -x)
# ─────────────────────────────────────────────────────────────────────────────

set -eu

# Enable debug mode if requested
if [ "${DEBUG:-0}" = "1" ]; then
    set -x
fi

# ─────────────────────────────────────────────────────────────────────────────
# Defaults
# ─────────────────────────────────────────────────────────────────────────────
LITERT_LM_DEPLOY_DIR="${LITERT_LM_DEPLOY_DIR:-/mnt/work/deploy/usr}"

echo "=== Building Rust cdylib wrappers ==="

# ─────────────────────────────────────────────────────────────────────────────
# Install Rust
# ─────────────────────────────────────────────────────────────────────────────
echo "Installing Rust toolchain..."
wget -q -O /tmp/rustup.sh \
    https://raw.githubusercontent.com/rust-lang/rustup/main/rustup-init.sh
sh /tmp/rustup.sh -y --default-toolchain stable --profile minimal
# shellcheck disable=SC1091
. "$HOME/.cargo/env"

# ─────────────────────────────────────────────────────────────────────────────
# Build llguidance cdylib wrapper
# ─────────────────────────────────────────────────────────────────────────────
echo "Building llguidance cdylib wrapper..."

# Find llguidance source in Bazel cache — prefer the external/ source tree
# (which has a real Cargo.toml) over bazel-out symlinks.
LLGUIDANCE_SRC=$(find "$HOME/.cache/bazel" -path "*/external/crate_index__llguidance-1.3.0" \
    ! -path "*/bazel-out/*" -type d 2>/dev/null | head -1)
# Fallback: any matching directory that contains Cargo.toml
if [ -z "$LLGUIDANCE_SRC" ]; then
    LLGUIDANCE_SRC=$(find "$HOME/.cache/bazel" -path "*/crate_index__llguidance-1.3.0" \
        -name "Cargo.toml" 2>/dev/null | head -1 | xargs -r dirname)
fi

if [ -n "$LLGUIDANCE_SRC" ]; then
    echo "Found llguidance source at: $LLGUIDANCE_SRC"

    mkdir -p /tmp/llg-build && cd /tmp/llg-build

    # Create Cargo.toml for cdylib wrapper
    cat > Cargo.toml <<EOF
[package]
name = "llguidance-cdylib"
version = "1.3.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
llguidance = { path = "${LLGUIDANCE_SRC}", default-features = false, features = ["ahash", "lark", "rayon"] }
EOF

    # Create minimal lib.rs that re-exports llguidance
    mkdir -p src
    echo 'pub use llguidance::*;' > src/lib.rs

    # Build release version
    cargo build --release

    # Copy to deployment directory
    CAPI_SO=$(find target/release -name "libllguidance_cdylib.so" -type f 2>/dev/null | head -1)
    if [ -n "$CAPI_SO" ]; then
        echo "Copying llguidance cdylib to ${LITERT_LM_DEPLOY_DIR}/lib/libllguidance.so"
        cp -avL "$CAPI_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libllguidance.so"

        # liblitert_lm_c_api.so has an undefined symbol llg_new_tokenizer that
        # must come from libllguidance.so. The dynamic linker only loads it if
        # it appears in NEEDED. Use patchelf to add the dependency.
        LITERT_LM_SO="${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so"
        if [ -f "$LITERT_LM_SO" ] && command -v patchelf >/dev/null 2>&1; then
            echo "Patching ${LITERT_LM_SO} to add NEEDED libllguidance.so"
            patchelf --add-needed libllguidance.so "$LITERT_LM_SO"
            echo "✓ patchelf done"
        else
            echo "WARNING: patchelf not available or liblitert_lm_c_api.so not found; llg_new_tokenizer may remain unresolved"
        fi
    else
        echo "WARNING: llguidance cdylib not found in build output"
    fi

    rm -rf /tmp/llg-build
else
    echo "WARNING: llguidance source not found in Bazel cache, skipping"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Fix RPATH for all shared libraries
# ─────────────────────────────────────────────────────────────────────────────
echo "Fixing RPATH for shared libraries..."

for so in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so; do
    if [ -f "$so" ]; then
        patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true
    fi
done

# ─────────────────────────────────────────────────────────────────────────────
# Build cxxbridge runtime cdylib
# liblitert_lm_c_api.so has undefined cxxbridge symbols (e.g. _ZN4rust10cxxbridge13StrC1EPKc)
# that must be provided by a shared library at runtime. Build a minimal cdylib
# that re-exports the cxx crate's bridge runtime.
# ─────────────────────────────────────────────────────────────────────────────
echo "Building cxxbridge runtime cdylib..."

CXX_SRC=$(find "$HOME/.cache/bazel" -path "*/crate_index__cxx-*" \
    ! -path "*/bazel-out/*" -name "Cargo.toml" 2>/dev/null | head -1 | xargs -r dirname)

if [ -n "$CXX_SRC" ]; then
    echo "Found cxx source at: $CXX_SRC"

    # The C++ bridge shims are in cxx.cc — compile them as a shared library
    CXX_CC="${CXX_SRC}/src/cxx.cc"
    CXX_INCLUDE="${CXX_SRC}/include"
    if [ -f "$CXX_CC" ]; then
        echo "Compiling cxx C++ runtime shims from $CXX_CC"
        g++ -O2 -fPIC -shared \
            -I"${CXX_INCLUDE}" \
            "$CXX_CC" \
            -o "${LITERT_LM_DEPLOY_DIR}/lib/libcxxbridge_rt.so" \
            -lstdc++ && echo "✓ libcxxbridge_rt.so compiled from C++ source"
    else
        # Fallback: build Rust cdylib
        mkdir -p /tmp/cxx-build && cd /tmp/cxx-build
        cat > Cargo.toml <<EOF
[package]
name = "cxxbridge-rt"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["cdylib"]

[dependencies]
cxx = { path = "${CXX_SRC}" }
EOF
        mkdir -p src
        echo 'pub use cxx::*;' > src/lib.rs
        cargo build --release 2>/dev/null || true
        RT_SO=$(find target/release -name "libcxxbridge_rt.so" -type f 2>/dev/null | head -1)
        if [ -n "$RT_SO" ]; then
            cp -avL "$RT_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libcxxbridge_rt.so"
        fi
        rm -rf /tmp/cxx-build
    fi

    LITERT_LM_SO="${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so"
    if [ -f "${LITERT_LM_DEPLOY_DIR}/lib/libcxxbridge_rt.so" ] && \
       [ -f "$LITERT_LM_SO" ] && command -v patchelf >/dev/null 2>&1; then
        patchelf --add-needed libcxxbridge_rt.so "$LITERT_LM_SO" 2>/dev/null || true
        echo "✓ patchelf cxxbridge_rt done"
    fi
else
    echo "WARNING: cxx source not found in Bazel cache, skipping cxxbridge runtime build"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
rm -f /tmp/rustup.sh

# Remove the Rust toolchain to reclaim disk space in the Docker layer.
# The .so files are already copied to LITERT_LM_DEPLOY_DIR.
echo "Removing Rust toolchain to free disk space..."
rm -rf "$HOME/.rustup" "$HOME/.cargo" 2>/dev/null || true

echo "✓ Rust cdylib wrappers build complete"
echo "  Libraries: ${LITERT_LM_DEPLOY_DIR}/lib"
