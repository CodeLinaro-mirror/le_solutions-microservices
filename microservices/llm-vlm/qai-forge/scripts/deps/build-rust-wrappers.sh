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

# Find llguidance source in Bazel cache
LLGUIDANCE_SRC=$(find "$HOME/.cache/bazel" -path "*/crate_index__llguidance-1.3.0" -type d 2>/dev/null | head -1)

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
    else
        echo "WARNING: llguidance cdylib not found in build output"
    fi

    rm -rf /tmp/llg-build
else
    echo "WARNING: llguidance source not found in Bazel cache, skipping"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Build tokenizers_c cdylib wrapper
# ─────────────────────────────────────────────────────────────────────────────
echo "Building tokenizers_c cdylib wrapper..."

# Find tokenizers_cpp and tokenizers sources in Bazel cache
TOKENIZERS_CPP_SRC=$(find "$HOME/.cache/bazel" -path "*/external/tokenizers_cpp" -type d 2>/dev/null | head -1)
TOKENIZERS_EXACT_SRC=$(find "$HOME/.cache/bazel" -path "*/crate_index__tokenizers-0.21.0" -type d 2>/dev/null | head -1)

if [ -n "$TOKENIZERS_CPP_SRC" ] && [ -n "$TOKENIZERS_EXACT_SRC" ] && [ -d "$TOKENIZERS_CPP_SRC/rust" ]; then
    echo "Found tokenizers_cpp at: $TOKENIZERS_CPP_SRC"
    echo "Found tokenizers at: $TOKENIZERS_EXACT_SRC"

    cd "$TOKENIZERS_CPP_SRC/rust"

    # Patch Cargo.toml to build as cdylib
    python3 -c "import re; content=open('Cargo.toml').read(); patched=re.sub(r'crate-type\s*=\s*\[.*?\]', 'crate-type = [\"cdylib\"]', content); patched=patched if patched!=content else (content+'\n[lib]\ncrate-type = [\"cdylib\"]\n' if '[lib]' not in content else content); open('Cargo.toml','w').write(patched)"

    # Add patch to use exact tokenizers version from Bazel cache
    printf '\n[patch.crates-io]\ntokenizers = { path = "%s" }\n' "$TOKENIZERS_EXACT_SRC" >> Cargo.toml

    # Fix pointer dereference issues in lib.rs
    sed -i 's/\*out_len = (\*handle)\.decode_str\.len();/*out_len = (\&(*handle).decode_str).len();/' src/lib.rs
    sed -i 's/\*out_len = (\*handle)\.id_to_token_result\.len();/*out_len = (\&(*handle).id_to_token_result).len();/' src/lib.rs

    # Build release version
    cargo build --release

    # Copy to deployment directory
    TOK_SO=$(find target/release -name "libtokenizers_c.so" -type f 2>/dev/null | head -1)
    if [ -n "$TOK_SO" ]; then
        echo "Copying tokenizers_c to ${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so"
        cp -avL "$TOK_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so"
    else
        echo "WARNING: tokenizers_c not found in build output"
    fi
else
    echo "WARNING: tokenizers_cpp or tokenizers source not found in Bazel cache, skipping"
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
# Cleanup
# ─────────────────────────────────────────────────────────────────────────────
rm -f /tmp/rustup.sh

echo "✓ Rust cdylib wrappers build complete"
echo "  Libraries: ${LITERT_LM_DEPLOY_DIR}/lib"
