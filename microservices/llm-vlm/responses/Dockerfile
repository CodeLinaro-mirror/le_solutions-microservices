# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# responses/Dockerfile
#
# Builds the Responses API server container.
#
# Build context: llm-vlm/  (parent directory containing all siblings)
# Usage (from llm-vlm/):
#   docker build -f responses/Dockerfile -t genai-responses-server .
#
# The build context must include both:
#   - qai-forge/  (shared SDK: Layers 2-4 + worker binaries + daemon)
#   - responses/   (Layer 1: Drogon HTTP server for Responses API)
#
# The Qualcomm AI Runtime Community SDK (QAIRT) is fetched directly from
# softwarecenter.qualcomm.com during the build — no external base image needed.
#
# fastrpc is built from source (github.com/quic/fastrpc) so that the DSP RPC
# path is set to /run/device-model (required for Qualcomm Linux devices).
#
# Drogon is built from source with all optional database/cache backends
# disabled (no PostgreSQL, MySQL, SQLite3, Redis, Brotli). This avoids the
# libpq dependency that the apt-packaged libdrogon-dev pulls in.
#
# Four-stage build for minimal image size:
#   Stage 0  (litert_builder):     Build LiteRT v2.1.5 from source (Bazel, AArch64).
#   Stage 0b (litert_lm_builder):  Build LiteRT-LM v0.13.0 on top of LiteRT.
#   Stage 1  (builder):            Compile everything; strip our own ELF artifacts.
#   Stage 2  (stager):             Cherry-pick only the required system .so files from
#                                  a full Debian environment; assemble /export tree.
#   Stage 3  (runtime):            gcr.io/distroless/cc-debian12 — COPY /export in.
#                                  No shell, no package manager, no apt cache.
#
#   Privilege drop: handled natively by the C++ server binary at startup.
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0: litert_lm_builder
#
# Builds LiteRT-LM v0.14.0 natively on AArch64 using Bazelisk.
# LiteRT-LM will build LiteRT as a dependency (no separate LiteRT stage needed).
# Outputs staged artifacts to /mnt/work/deploy/usr:
#   /mnt/work/deploy/usr/lib/     — liblitert_lm_c_api.so, liblitert_runtime_c_api.so,
#                                    dispatch .so, plugin .so
#   /mnt/work/deploy/usr/bin/     — litert-lm, litert-lm-advanced
#   /mnt/work/deploy/usr/include/ — litert_lm/c/ and litert/ and tflite/ C API headers
#
# Runs natively on ARM64 — no cross-compilation required.
# Artifacts are copied into the ARM64 builder stage below.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS litert_lm_builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-15 \
        lld \
        lld-15 \
        llvm \
        g++ \
        wget \
        git \
        git-lfs \
        unzip \
        zip \
        python3 \
        python3-dev \
        python3-numpy \
        python-is-python3 \
        ca-certificates \
        patch \
        pkg-config \
        file \
        default-jdk-headless \
        binutils \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Use ARM64 Bazelisk for native AArch64 build
ENV BAZELISK_VERSION=v1.28.1
RUN wget -t 3 -T 120 --no-verbose \
        -O /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64" \
    && chmod +x /usr/local/bin/bazel

ENV LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr
# Clone LiteRT-LM at tag v0.14.0 for a stable, reproducible build.
RUN git lfs install \
    && for i in {1..5}; do \
        git clone --branch v0.14.0 --depth 1 \
            https://github.com/google-ai-edge/LiteRT-LM.git "${LITERT_LM_SRC_DIR}" && break || { \
            echo "Clone failed, retrying in 15 seconds..."; \
            rm -rf "${LITERT_LM_SRC_DIR}"; \
            sleep 15; \
        }; \
    done \
    && cd "${LITERT_LM_SRC_DIR}" \
    && git lfs pull

# ── Download QAIRT 2.45.40.260406 for LiteRT QNN dispatch + compiler plugin ──
# LiteRT (built as a dependency of LiteRT-LM) defaults to QAIRT 2.44.0.260225.
# We create a compatibility symlink: qairt/2.44.0.260225 → qairt/2.45.40.260406
# so that both LiteRT and the main builder use the same QAIRT version.
ENV QAIRT_VERSION=2.45.40.260406
ENV QAIRT_DIR=/tmp/qairt
RUN mkdir -p ${QAIRT_DIR} \
    && wget -t 3 -T 120 --no-verbose \
        -P ${QAIRT_DIR} \
        "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip" \
    && cd ${QAIRT_DIR} \
    && unzip v${QAIRT_VERSION}.zip \
    && rm -f v${QAIRT_VERSION}.zip \
    && ln -sf ${QAIRT_DIR}/qairt/${QAIRT_VERSION} \
              ${QAIRT_DIR}/qairt/2.44.0.260225 \
    && echo "QAIRT ${QAIRT_VERSION} ready; symlink 2.44.0.260225 → ${QAIRT_VERSION}"

# LITERT_QAIRT_SDK must end with "/" for Bazel's configurable_repo path concatenation.
ENV LITERT_QAIRT_SDK=${QAIRT_DIR}/

WORKDIR ${LITERT_LM_SRC_DIR}

# ── Add cc_shared_library target for the LiteRT-LM C API ──────────────────────
# cc_shared_library statically links all transitive deps and exports the
# symbols from targets listed in exports_filter, producing a self-contained
# .so that CMake consumers can link against without needing companion .so files.
# The deps = [":engine"] should transitively include the llguidance rlib through
# the normal dependency chain. Without --define=resolve_symbols_in_exec=false,
# the linker should embed the llg_* symbols into the .so.
ARG CACHE_BUSTER=2
RUN printf '\n# Shared library target for the LiteRT-LM C API.\n# Produces liblitert_lm_c_api.so that can be loaded at runtime.\n# Used by litert-lm-inference-worker in the responses service.\ncc_shared_library(\n    name = "litert_lm_c_api",\n    exports_filter = ["//c:engine"],\n    deps = [":engine"],\n    user_link_flags = ["-Wl,--export-dynamic"],\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD \
    && echo "Added cc_shared_library target to c/BUILD" \
    && echo "Skipping Bazel cc_shared_library for tokenizers_cpp (will build with Cargo)"

# Build LiteRT-LM using Bazel. LiteRT-LM will build LiteRT as a dependency.
# LITERT_QAIRT_SDK points to QAIRT 2.45.40.260406 with the 2.44.0.260225 symlink.
# Use clang-15 (not clang-14) for std::source_location support in C++20.
# The --define flags:
#   litert_enable_qnn=true: Enable QNN backend support
#   litert_link_capi_so=true: Link the constraint provider into the C API .so
# NOTE: --define=resolve_symbols_in_exec=false is REMOVED. This flag was causing
# the llg_* symbols to be left unresolved (UND) in the .so, expecting the loading
# executable to provide them. Since our litert-lm-inference-worker is built by
# CMake (not Bazel), it never links the Rust rlib. Without this flag, the
# cc_shared_library rule should transitively link the llguidance rlib and embed
# the symbols into the .so.
RUN bazel build \
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

# Install patchelf to fix Bazel-embedded RUNPATH after build
RUN apt-get update && apt-get install -y --no-install-recommends patchelf \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Stage LiteRT-LM shared library, C API headers, and extract LiteRT artifacts.
RUN mkdir -p "${LITERT_LM_DEPLOY_DIR}/lib" \
             "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c" \
             "${LITERT_LM_DEPLOY_DIR}/include/litert" \
             "${LITERT_LM_DEPLOY_DIR}/include/tflite" \
    && echo "Searching for litert_lm_c_api .so in bazel outputs:" \
    && find -L bazel-bin bazel-out \( -type f -o -type l \) \
        \( -name "liblitert_lm_c_api.so" -o -name "liblitert_lm_c_api.so.*" \
           -o -name "litert_lm_c_api.so" -o -name "litert_lm_c_api.so.*" \
           -o -name "libGemmaModelConstraintProvider.so" \
           -o -name "libllguidance*.so" -o -name "libllguidance*.so.*" \
           -o -name "liblitert_lm_llguidance*.so" -o -name "liblitert_lm_llguidance*.so.*" \) \
        2>/dev/null | tee /tmp/litert_lm_so_files.txt \
    && echo "Copying LiteRT-LM .so files:" \
    && while IFS= read -r f; do \
            base=$(basename "$f"); \
            if echo "$base" | grep -q "^lib"; then \
                cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/" || true; \
            else \
                cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/lib${base}" || true; \
            fi; \
       done < /tmp/litert_lm_so_files.txt \
    && echo "Resolving NEEDED dependencies of liblitert_lm_c_api.so:" \
    && if [ -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" ]; then \
           readelf -d "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
               | grep NEEDED | sed 's/.*\[//;s/\]//' \
               | while IFS= read -r needed_lib; do \
                   echo "  NEEDED: $needed_lib"; \
                   if [ ! -f "${LITERT_LM_DEPLOY_DIR}/lib/$needed_lib" ]; then \
                       found=$(find -L bazel-bin bazel-out -name "$needed_lib" 2>/dev/null | head -1); \
                       if [ -n "$found" ]; then \
                           echo "  Staging from Bazel: $found"; \
                           cp -avL "$found" "${LITERT_LM_DEPLOY_DIR}/lib/"; \
                       else \
                           echo "  Not in Bazel outputs (system lib): $needed_lib"; \
                       fi; \
                   else \
                       echo "  Already staged: $needed_lib"; \
                   fi; \
               done; \
       fi \
    && cp c/engine.h "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c/" \
    && echo "Extracting LiteRT artifacts from Bazel external directory:" \
    && LITERT_EXTERNAL=$(find bazel-* -type d -path "*/external/litert" 2>/dev/null | head -1) \
    && if [ -n "$LITERT_EXTERNAL" ] && [ -d "$LITERT_EXTERNAL" ]; then \
           echo "Found LiteRT external at: $LITERT_EXTERNAL"; \
           find "$LITERT_EXTERNAL" -type f \( -name "*.so" -o -name "*.so.*" \) \
               -exec cp -avL {} "${LITERT_LM_DEPLOY_DIR}/lib/" \; 2>/dev/null || true; \
           if [ -d "$LITERT_EXTERNAL/litert" ]; then \
               cp -a "$LITERT_EXTERNAL/litert" "${LITERT_LM_DEPLOY_DIR}/include/" || true; \
           fi; \
           if [ -d "$LITERT_EXTERNAL/tflite" ]; then \
               cp -a "$LITERT_EXTERNAL/tflite" "${LITERT_LM_DEPLOY_DIR}/include/" || true; \
           fi; \
       else \
           echo "Warning: LiteRT external directory not found in Bazel cache"; \
       fi \
    && echo "Staged LiteRT-LM library:" \
    && ls -lah "${LITERT_LM_DEPLOY_DIR}/lib/" \
    && test -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
    && echo "✓ liblitert_lm_c_api.so staged successfully"


# ── Build Rust cdylib wrappers for FFI (Cargo) ───────────────────────────────
# The cc_shared_library rule does not compile the Rust FFI symbols because it
# does not build the rlib dependencies as cdylibs. We must build them separately
# using Cargo, then stage them alongside liblitert_lm_c_api.so.
# We build against the Bazel-cached source to ensure all LiteRT-LM patches are applied.
RUN wget -q -O /tmp/rustup.sh https://sh.rustup.rs \
    && sh /tmp/rustup.sh -y --default-toolchain stable --profile minimal \
    && . /root/.cargo/env \
    \
    && echo "--- Building libllguidance.so ---" \
    && LLGUIDANCE_SRC=$(find /root/.cache/bazel -path "*/crate_index__llguidance-1.3.0" -type d 2>/dev/null | head -1) \
    && if [ -z "$LLGUIDANCE_SRC" ]; then \
           echo "ERROR: llguidance source not found in Bazel cache"; \
           exit 1; \
       fi \
    && echo "Found llguidance source at: $LLGUIDANCE_SRC" \
    && mkdir -p /tmp/llg-build \
    && cd /tmp/llg-build \
    && echo "Creating wrapper Cargo project to build llguidance..." \
    && printf '[package]\nname = "llguidance-cdylib"\nversion = "1.3.0"\nedition = "2021"\n\n[lib]\ncrate-type = ["cdylib"]\n\n[dependencies]\nllguidance = { path = "%s", default-features = false, features = ["ahash", "lark", "rayon"] }\n' "$LLGUIDANCE_SRC" > Cargo.toml \
    && mkdir -p src \
    && echo 'pub use llguidance::*;' > src/lib.rs \
    && echo "Building libllguidance_cdylib.so..." \
    && cargo build --release \
    && CAPI_SO=$(find target/release -name "libllguidance_cdylib.so" -type f 2>/dev/null | head -1) \
    && if [ -z "$CAPI_SO" ]; then \
           echo "ERROR: cargo build failed to produce libllguidance_cdylib.so"; \
           exit 1; \
       fi \
    && echo "Staging libllguidance.so..." \
    && cp -avL "$CAPI_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libllguidance.so" \
    && echo "✓ libllguidance.so built and staged successfully" \
    && rm -rf /tmp/llg-build \
    \
    && echo "--- Building libhuggingface_tokenizer.so from tokenizers_cpp ---" \
    && TOKENIZERS_CPP_SRC=$(find /root/.cache/bazel -path "*/external/tokenizers_cpp" -type d 2>/dev/null | head -1) \
    && if [ -z "$TOKENIZERS_CPP_SRC" ]; then \
           echo "ERROR: tokenizers_cpp external not found in Bazel cache"; \
           exit 1; \
       fi \
    && echo "Found tokenizers_cpp at: $TOKENIZERS_CPP_SRC" \
    && if [ ! -d "$TOKENIZERS_CPP_SRC/rust" ] || [ ! -f "$TOKENIZERS_CPP_SRC/rust/Cargo.toml" ]; then \
           echo "ERROR: tokenizers_cpp/rust directory or Cargo.toml not found"; \
           exit 1; \
       fi \
    && TOKENIZERS_EXACT_SRC=$(find /root/.cache/bazel -path "*/crate_index__tokenizers-0.21.0" -type d 2>/dev/null | head -1) \
    && if [ -z "$TOKENIZERS_EXACT_SRC" ]; then \
           echo "ERROR: tokenizers v0.21.0 source not found in Bazel cache"; \
           exit 1; \
       fi \
    && echo "Found tokenizers v0.21.0 at: $TOKENIZERS_EXACT_SRC" \
    && cd "$TOKENIZERS_CPP_SRC/rust" \
    && echo "Patching Cargo.toml to change crate-type to cdylib and pin tokenizers to v0.21.0..." \
    && python3 -c "import re; content=open('Cargo.toml').read(); patched=re.sub(r'crate-type\s*=\s*\[.*?\]', 'crate-type = [\"cdylib\"]', content); patched=patched if patched!=content else (content+'\n[lib]\ncrate-type = [\"cdylib\"]\n' if '[lib]' not in content else content); open('Cargo.toml','w').write(patched)" \
    && printf '\n[patch.crates-io]\ntokenizers = { path = "%s" }\n' "$TOKENIZERS_EXACT_SRC" >> Cargo.toml \
    && echo "Patched Cargo.toml [lib] section:" \
    && grep -A 2 '\[lib\]' Cargo.toml || echo "WARNING: No [lib] section found" \
    && echo "Patching src/lib.rs to fix implicit autoref errors..." \
    && sed -i 's/\*out_len = (\*handle)\.decode_str\.len();/*out_len = (\&(*handle).decode_str).len();/' src/lib.rs \
    && sed -i 's/\*out_len = (\*handle)\.id_to_token_result\.len();/*out_len = (\&(*handle).id_to_token_result).len();/' src/lib.rs \
    && echo "Building tokenizers_cpp Rust FFI wrapper as cdylib..." \
    && cargo build --release \
    && echo "Cargo build artifacts produced:" \
    && find target/release -maxdepth 1 \( -name "*.so" -o -name "*.rlib" -o -name "libtokenizers*" \) -type f 2>/dev/null | head -10 \
    && TOK_SO=$(find target/release -name "libtokenizers_c.so" -type f 2>/dev/null | head -1) \
    && if [ -z "$TOK_SO" ]; then \
           echo "ERROR: cargo build failed to produce libtokenizers_c.so"; \
           exit 1; \
       fi \
    && echo "Staging libtokenizers_c.so..." \
    && cp -avL "$TOK_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so" \
    && echo "Verifying tokenizers_* symbols in libtokenizers_c.so:" \
    && if ! nm -D "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so" | grep -q " T tokenizers_"; then \
           echo "ERROR: No tokenizers_* symbols found in libtokenizers_c.so"; \
           echo "Symbol table:"; \
           nm -D "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so" | head -20; \
           exit 1; \
       fi \
    && echo "Found tokenizers_* symbols:" \
    && nm -D "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so" | grep " T tokenizers_" | head -5 \
    && echo "✓ libtokenizers_c.so built and staged successfully"

# ── Patch Bazel RUNPATH out of ALL staged .so files ──────────────────────────
# Bazel embeds sandbox-relative RUNPATH entries (_solib_aarch64/..., .runfiles/...)
# that don't exist at runtime on the device. Replace with $ORIGIN so the dynamic
# linker finds companion libraries (e.g., LLGuidance) in the same directory (/usr/lib/).
RUN for so in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so; do \
        [ -f "$so" ] && patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true; \
    done \
    && echo "Patched RUNPATH for all staged .so files:" \
    && readelf -d "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
        | grep -E 'NEEDED|RUNPATH|RPATH'


# ── Verify library dependencies at build time ────────────────────────────────
# Confirms all NEEDED libraries are present. Note that liblitert_lm_c_api.so
# will have UND llg_* symbols by design. At runtime, the dynamic linker will
# resolve these from the executable or dynamically loaded libraries.
RUN echo "Verifying liblitert_lm_c_api.so NEEDED dependencies:" \
    && export LD_LIBRARY_PATH="${LITERT_LM_DEPLOY_DIR}/lib:$LD_LIBRARY_PATH" \
    && ldd "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" | tee /tmp/ldd_litert_lm.txt \
    && if grep -q "not found" /tmp/ldd_litert_lm.txt; then \
           echo "ERROR: Unresolved library dependencies:"; \
           grep "not found" /tmp/ldd_litert_lm.txt; \
           exit 1; \
       fi \
    && echo "✓ All liblitert_lm_c_api.so NEEDED dependencies are present" \
    && echo "Scanning all staged libraries for unresolved Rust-like symbols..." \
    && UNRESOLVED_FOUND=false \
    && for so in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so*; do \
           [ -f "$so" ] || continue; \
           # Look for UND symbols that match known Rust FFI prefixes
           for sym in $(nm -D "$so" 2>/dev/null | awk '$1 == "U" {print $2}' | grep -E '^(llg|toktrie|tokenizers|cxxbridge1)_'); do \
               # Check if any OTHER staged library provides this symbol
               resolved=false; \
               for provider in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so*; do \
                   if nm -D "$provider" 2>/dev/null | grep -q " T $sym"; then \
                       resolved=true; \
                       break; \
                   fi; \
               done; \
               if [ "$resolved" = "false" ]; then \
                   echo "  WARNING: Unresolved symbol '$sym' in $(basename "$so")"; \
                   UNRESOLVED_FOUND=true; \
               fi; \
           done; \
       done \
    && if [ "$UNRESOLVED_FOUND" = "true" ]; then \
           echo "⚠️ WARNING: Found unresolved Rust FFI symbols. These may cause runtime errors."; \
           echo "If you encounter runtime 'undefined symbol' errors, you may need to build"; \
           echo "and stage a cdylib wrapper for the corresponding Rust crate."; \
       else \
           echo "✓ No unresolved Rust FFI symbols found."; \
       fi

# ── Detailed diagnostic for libGemmaModelConstraintProvider.so ───────────────
# This library is the constraint provider that uses llguidance. Check whether
# llg_new_tokenizer and other llg_* symbols are embedded (T) or external (U).
RUN echo "=== Detailed diagnostic for libGemmaModelConstraintProvider.so ===" \
    && if [ -f "${LITERT_LM_DEPLOY_DIR}/lib/libGemmaModelConstraintProvider.so" ]; then \
           echo "Library dependencies (ldd):"; \
           export LD_LIBRARY_PATH="${LITERT_LM_DEPLOY_DIR}/lib:$LD_LIBRARY_PATH"; \
           ldd "${LITERT_LM_DEPLOY_DIR}/lib/libGemmaModelConstraintProvider.so" || true; \
           echo ""; \
           echo "llg_* symbols (T=defined, U=undefined):"; \
           nm -D "${LITERT_LM_DEPLOY_DIR}/lib/libGemmaModelConstraintProvider.so" \
               | grep " llg_" | head -20 || echo "  (no llg_* symbols found)"; \
           echo ""; \
           echo "Checking for unresolved llg_* symbols:"; \
           UNRESOLVED_LLG=false; \
           for sym in $(nm -D "${LITERT_LM_DEPLOY_DIR}/lib/libGemmaModelConstraintProvider.so" 2>/dev/null \
                        | awk '$1 == "U" && $2 ~ /^llg_/ {print $2}'); do \
               resolved=false; \
               for provider in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so*; do \
                   if nm -D "$provider" 2>/dev/null | grep -q " T $sym"; then \
                       echo "  ✓ $sym resolved by $(basename "$provider")"; \
                       resolved=true; \
                       break; \
                   fi; \
               done; \
               if [ "$resolved" = "false" ]; then \
                   echo "  ✗ ERROR: $sym is UND and not provided by any staged library"; \
                   UNRESOLVED_LLG=true; \
               fi; \
           done; \
           if [ "$UNRESOLVED_LLG" = "true" ]; then \
               echo "ERROR: libGemmaModelConstraintProvider.so has unresolved llg_* symbols"; \
               exit 1; \
           else \
               echo "✓ All llg_* symbols in libGemmaModelConstraintProvider.so are resolved"; \
           fi; \
       else \
           echo "WARNING: libGemmaModelConstraintProvider.so not found in staged libraries"; \
       fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 debian:bookworm-slim AS builder

# Install build dependencies:
#   - Drogon hard deps (no DB client libs)
#   - fastrpc build deps: autoconf/automake/libtool/libbsd-dev/libmd-dev/libyaml-dev
#   - binutils for strip
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        wget \
        unzip \
        ca-certificates \
        libssl-dev \
        libjsoncpp-dev \
        uuid-dev \
        zlib1g-dev \
        nlohmann-json3-dev \
        autoconf \
        automake \
        libtool \
        m4 \
        libbsd-dev \
        libmd-dev \
        libyaml-dev \
        binutils \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# ── Deploy directory (all runtime artifacts staged here) ─────────────────────
# Defined early so all subsequent build steps can install directly into it.
ENV DEPLOY_DIR=/build/deploy

# ── Build fastrpc from source ─────────────────────────────────────────────────
# Sets MACHINE_NAME_PATH to /run/device-model (required for Qualcomm Linux).
# Provides libcdsprpc.so and libadsprpc.so used by libGenie.so at runtime.
RUN git clone --depth 1 https://github.com/quic/fastrpc.git /tmp/fastrpc && \
    cd /tmp/fastrpc && \
    autoreconf -is && \
    CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
        ./configure --prefix=/usr --libdir=/usr/lib && \
    make -j$(nproc) && \
    make install && \
    rm -rf /tmp/fastrpc

# ── Build lightweight Drogon from source ──────────────────────────────────────
# Installed directly into ${DEPLOY_DIR}/usr so its shared libraries
# (libdrogon.so, libtrantor.so) land in the deploy tree.
RUN git clone --depth 1 --branch v1.9.6 \
        https://github.com/drogonframework/drogon.git /tmp/drogon && \
    cd /tmp/drogon && \
    git submodule update --init && \
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_CTL=OFF \
        -DBUILD_TESTING=OFF \
        -DUSE_MYSQL=OFF \
        -DUSE_SQLITE3=OFF \
        -DUSE_POSTGRESQL=OFF \
        -DUSE_REDIS=OFF \
        -DUSE_BROTLI=OFF \
        -DUSE_SPDLOG=OFF \
        -DUSE_YAML_CPP=OFF && \
    cmake --build build -j$(nproc) && \
    cmake --install build && \
    rm -rf /tmp/drogon

# ── Fetch and install Qualcomm AI Runtime Community (QAIRT) SDK ───────────────
# Downloads the SDK zip from softwarecenter.qualcomm.com, extracts:
#   - Genie headers  → /usr/include/  (for build)
#   - libGenie.so    → /usr/lib/      (for build)
#   - All aarch64 runtime libs + libGenie.so → ${DEPLOY_DIR}/usr/lib/
#   - Hexagon DSP blobs (v68/v73/v75) → ${DEPLOY_DIR}/usr/lib/rfsa/adsp/
ENV QAIRT_VERSION=2.45.40.260406
ENV QAIRT_DOWNLOAD_DIR=/tmp/qairt-download
ENV QAIRT_LIB_PATH=${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/lib

RUN mkdir -p ${QAIRT_DOWNLOAD_DIR} \
             ${DEPLOY_DIR}/usr/lib/rfsa/adsp \
             ${DEPLOY_DIR}/usr/include && \
    wget -t 2 -T 30 -P ${QAIRT_DOWNLOAD_DIR} \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip && \
    cd ${QAIRT_DOWNLOAD_DIR} && \
    unzip v${QAIRT_VERSION}.zip && \
    rm -f v${QAIRT_VERSION}.zip && \
    find ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/ \
        -type f -iname "*snpe*" -exec rm {} \; && \
    cp -r ${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/Genie/* \
        /usr/include/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so \
        /usr/lib/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v68/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v73/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v75/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so \
        ${DEPLOY_DIR}/usr/lib/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/* \
        ${DEPLOY_DIR}/usr/lib/ && \
    rm -f \
        ${DEPLOY_DIR}/usr/lib/libQnnGpu.so \
        ${DEPLOY_DIR}/usr/lib/libQnnGpuNetRunExtensions.so \
        ${DEPLOY_DIR}/usr/lib/libQnnGpuProfilingReader.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV68CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV69CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV69Stub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV73CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV75CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV79CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV79Stub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV81CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnHtpV81Stub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnLpai.so \
        ${DEPLOY_DIR}/usr/lib/libQnnLpaiNetRunExtensions.so \
        ${DEPLOY_DIR}/usr/lib/libQnnLpaiProfilingReader.so \
        ${DEPLOY_DIR}/usr/lib/libQnnLpaiStub.so \
        ${DEPLOY_DIR}/usr/lib/libQnnTFLiteDelegate.so \
        ${DEPLOY_DIR}/usr/lib/libSNPE.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeDspV66Stub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHta.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpPrepare.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV68CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV68Stub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV73CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV73Stub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV75CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV75Stub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV79CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV79Stub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV81CalculatorStub.so \
        ${DEPLOY_DIR}/usr/lib/libSnpeHtpV81Stub.so \
        ${DEPLOY_DIR}/usr/lib/libcalculator.so \
        ${DEPLOY_DIR}/usr/lib/libhta_hexagon_runtime_snpe.so && \
    rm -rf ${QAIRT_DOWNLOAD_DIR}/qairt

# Copy fastrpc runtime libraries into deploy staging
RUN cp /usr/lib/libcdsprpc.so* ${DEPLOY_DIR}/usr/lib/ && \
    cp /usr/lib/libadsprpc.so* ${DEPLOY_DIR}/usr/lib/

# ── Copy LiteRT-LM and LiteRT libraries (built together) ──────────────────────
# LiteRT-LM built LiteRT as a dependency, so all artifacts are in one place.
# The .so files land in /usr/lib/ for build-time linking.
# Headers land in /usr/include/ for qai-forge compilation.
# Note: CLI tools (litert-lm, litert-lm-advanced) are not built; only the C API library.
# The litert-lm-inference-worker is built by qai-forge, not by LiteRT-LM.
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib /usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib ${DEPLOY_DIR}/usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/include /usr/include

# ── Build qai-forge (shared SDK: Layers 2-4 + worker binaries) ───────────────
WORKDIR /build
COPY qai-forge /build/qai-forge

RUN cmake -S /build/qai-forge -B /build/qai-forge/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENIE_INCLUDE_PATH=/usr/include \
        -DGENIE_LIB_PATH=/usr/lib \
        -DQAI_FORGE_BUILD_LITERT_LM=ON \
        -DLITERT_LM_INCLUDE_PATH=/usr/include \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/qai-forge/build -j$(nproc) && \
    cmake --install /build/qai-forge/build --prefix ${DEPLOY_DIR}/usr

# ── Build responses server (Layer 1: Drogon HTTP server) ──────────────────────
COPY responses /build/responses

RUN cmake -S /build/responses -B /build/responses/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/responses/build -j$(nproc) && \
    cmake --install /build/responses/build

# ── Verify LiteRT-LM dependencies before stripping ───────────────────────────
# Check that liblitert_lm_c_api.so has all required symbols resolved.
# This catches missing llguidance libraries early in the build process.
RUN echo "Verifying LiteRT-LM library dependencies:" \
    && export LD_LIBRARY_PATH=${DEPLOY_DIR}/usr/lib:/usr/lib:$LD_LIBRARY_PATH \
    && if [ -f "${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so" ]; then \
           ldd ${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so || { \
               echo "ERROR: liblitert_lm_c_api.so has unresolved dependencies"; \
               echo "Available .so files in ${DEPLOY_DIR}/usr/lib/:"; \
               ls -lh ${DEPLOY_DIR}/usr/lib/*.so* 2>/dev/null || true; \
               exit 1; \
           }; \
       fi \
    && echo "✓ LiteRT-LM dependencies verified"

# ── Strip our own compiled ELF artifacts to reduce size ──────────────────────
# Strips debug symbols from:
#   - All executables in bin/ (our server + worker binaries)
#   - Our own compiled .so files (Drogon, trantor, qai_forge, fastrpc)
# Deliberately skips ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ — those are Hexagon DSP
# blobs compiled for the DSP core, not ARM64; stripping them would corrupt them.
# Vendor QAIRT .so files are already stripped release binaries; strip is a no-op.
RUN find ${DEPLOY_DIR}/usr/bin -type f \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: Stager
#
# Installs runtime apt packages solely to obtain their .so files, then
# cherry-picks only the required libraries into /export. Nothing else from
# this stage's OS lands in the final image.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 debian:bookworm-slim AS stager

# Install runtime deps to make their .so files available for cherry-picking.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libyaml-0-2 \
        libbsd0 \
        libmd0 \
        libjsoncpp25 \
        libssl3 \
        zlib1g \
        libuuid1 \
        libatomic1 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Assemble the /export tree that will be COPY'd into the distroless runtime.
RUN mkdir -p /export/usr/bin \
             /export/usr/lib/rfsa/adsp \
             /export/etc/fastrpc

# Copy stripped binaries from the builder deploy tree.
# Deliberately excludes qai-forge-daemon — the HTTP server spawns workers directly.
COPY --from=builder /build/deploy/usr/bin/responses-server /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/genai-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/genai-vlm-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/litert-lm-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/lib /export/usr/lib

# Cherry-pick only the required system .so files (with symlinks preserved).
# These are the libraries NOT present in gcr.io/distroless/cc-debian12:
#   fastrpc deps:  libyaml-0-2, libbsd, libmd
#   Drogon deps:   libjsoncpp, libssl, libcrypto, libz, libuuid
#   C++ atomics:   libatomic
# distroless/cc already provides: libc, libstdc++, libgcc_s, libm
#
# Uses find to locate .so files regardless of whether Debian placed them under
# /usr/lib/aarch64-linux-gnu/, /usr/lib/, or /lib/ — all three are searched.
RUN for lib in libyaml-0 libbsd libmd libjsoncpp libssl libcrypto libz libuuid libatomic; do \
      find /usr/lib /lib -name "${lib}.so*" -exec cp -dp {} /export/usr/lib/ \; ; \
    done

# Bake fastrpc DSP path config for all supported Qualcomm devices.
RUN { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo '# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.'; \
      echo 'machines:'; \
      echo '  Qualcomm Technologies, Inc. Monaco Monza addons:'; \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp'; \
      echo '  Qualcomm Technologies, Inc. Addons IQ 9075 EVK:'; \
      echo '    DSP_LIBRARY_PATH: sa8775p/Qualcomm/SA8775P-RIDE/dsp'; \
      echo '  Qualcomm Technologies, Inc. IQ8 8275 Pro SKU EVK:'; \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp'; \
    } > /export/etc/fastrpc/hexagon-dsp-binaries.yaml

# ─────────────────────────────────────────────────────────────────────────────
# Stage 3: Runtime (distroless)
#
# gcr.io/distroless/cc-debian12 provides only:
#   libc6, libstdc++6, libgcc-s1, libm6 — nothing else.
# No shell, no package manager, no apt cache, no /usr/share docs.
# All additional .so files were cherry-picked into /export by the stager.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 gcr.io/distroless/cc-debian12 AS responses-runtime

# Single COPY brings in:
#   /usr/bin/   - responses-server, genai-inference-worker,
#                 genai-vlm-inference-worker, litert-lm-inference-worker
#   /usr/lib/   - libqai_forge.so, libdrogon.so, libtrantor.so,
#                 libGenie.so, QNN DSP/HTP/HTA aarch64 libs (GPU/LPAI/SNPE excluded),
#                 libcdsprpc.so*, libadsprpc.so*,
#                 liblitert_runtime_c_api.so, LiteRT dispatch + compiler plugin .so,
#                 liblitert_lm_c_api.so,
#                 cherry-picked system .so files
#   /usr/lib/rfsa/adsp/ - Hexagon DSP blobs (v68/v73/v75)
#   /etc/fastrpc/       - hexagon-dsp-binaries.yaml
COPY --from=stager /export /

# Set environment defaults
ENV RESPONSES_PORT=9002
ENV GENAI_MODELS_DIR=/mnt/work/models
ENV GENAI_WORKER_BINARY=/usr/bin/genai-inference-worker
ENV GENAI_VLM_WORKER_BINARY=/usr/bin/genai-vlm-inference-worker
ENV LITERT_LM_WORKER_BINARY=/usr/bin/litert-lm-inference-worker
ENV LITERT_LM_BACKEND=npu
ENV LOG_LEVEL=ERROR
# Watchdog timeouts — override in docker-compose.yaml as needed
ENV GENAI_WORKER_IDLE_TIMEOUT=120
ENV GENAI_WORKER_ACTIVE_TIMEOUT=600

EXPOSE 9002

WORKDIR /mnt/work/models

# The C++ server binary is the entrypoint.
# It calls drop_privileges_and_bind_devices() before starting Drogon.
# Child processes (genai-inference-worker) inherit the dropped privileges.
ENTRYPOINT ["/usr/bin/responses-server"]

# ADSP library path for Hexagon DSP firmware lookup at runtime.
# The DSP blobs are staged at /usr/lib/rfsa/adsp/ by the stager.
ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
