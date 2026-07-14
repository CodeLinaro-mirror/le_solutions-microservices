# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# Build arguments for modular backend configuration
# ─────────────────────────────────────────────────────────────────────────────
ARG BUILD_LITERT_LM=true
ARG BUILD_LLAMACPP=false

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
#   Stage 0  (litert_lm_builder):  Build LiteRT-LM v0.14.0 (conditional via shell if)
#   Stage 1  (llamacpp_builder):   Build llama.cpp with Hexagon NPU (conditional via shell if)
#   Stage 2  (builder):            Compile everything; strip our own ELF artifacts.
#   Stage 3  (stager):             Cherry-pick only the required system .so files from
#                                  a full Debian environment; assemble /export tree.
#   Stage 4  (runtime):            gcr.io/distroless/cc-debian12 — COPY /export in.
#                                  No shell, no package manager, no apt cache.
#
#   Privilege drop: handled natively by the C++ server binary at startup.
#
# ─────────────────────────────────────────────────────────────────────────────
# Base Image Strategy (TEMPORARY):
# ─────────────────────────────────────────────────────────────────────────────
# Current state:
#   - litert_lm_builder:  ubuntu:22.04 (temporary, due to image availability)
#   - llamacpp_builder:   ubuntu:22.04 (temporary, due to image availability)
#   - builder:            ubuntu:22.04 (temporary, due to image availability)
#   - stager:             ubuntu:22.04 (temporary, due to image availability)
#   - runtime:            gcr.io/distroless/cc-debian12
#
# Future plan:
#   Migrate all builder and stager stages to debian:bookworm-slim for consistency
#   once the image is available on the build host. This will provide:
#   - Consistent glibc version across all build stages (2.36)
#   - Simplified dependency management
#   - Reduced image size (bookworm-slim is smaller than ubuntu:22.04)
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0: LiteRT-LM Builder (conditional via shell if)
#
# Builds LiteRT-LM v0.14.0 natively on AArch64 using Bazelisk when BUILD_LITERT_LM=true.
# When BUILD_LITERT_LM=false, only creates minimal directory structure (zero build cost).
#
# This stage uses shell conditionals (if [ "$BUILD_LITERT_LM" = "true" ]; then ...; fi)
# instead of dynamic FROM statements, making it compatible with classic Docker builder.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS litert_lm_builder

# Re-declare ARG after FROM (required for ARG to be available in this stage)
ARG BUILD_LITERT_LM=false

# Always create minimal directory structure (required for COPY --from= to succeed)
RUN mkdir -p /mnt/work/deploy/usr/lib /mnt/work/deploy/usr/include /mnt/work/deploy/usr/bin

# Conditional build: Only execute expensive operations when BUILD_LITERT_LM=true
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        echo "=== Building LiteRT-LM (BUILD_LITERT_LM=true) ===" && \
        apt-get update && apt-get install -y --no-install-recommends \
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
            patchelf \
        && apt-get clean \
        && rm -rf /var/lib/apt/lists/*; \
    else \
        echo "=== Skipping LiteRT-LM build (BUILD_LITERT_LM=false) ==="; \
    fi

# Download Bazelisk (only if BUILD_LITERT_LM=true)
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        export BAZELISK_VERSION=v1.28.1 && \
        wget -t 3 -T 120 --no-verbose \
            -O /usr/local/bin/bazel \
            "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64" \
        && chmod +x /usr/local/bin/bazel; \
    fi

# Clone LiteRT-LM and download QAIRT (only if BUILD_LITERT_LM=true)
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        export LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm && \
        export QAIRT_VERSION=2.45.40.260406 && \
        export QAIRT_DIR=/tmp/qairt && \
        git lfs install \
        && for i in {1..5}; do \
            git clone --branch v0.14.0 --depth 1 \
                https://github.com/google-ai-edge/LiteRT-LM.git "${LITERT_LM_SRC_DIR}" && break || { \
                echo "Clone failed, retrying in 15 seconds..."; \
                rm -rf "${LITERT_LM_SRC_DIR}"; \
                sleep 15; \
            }; \
        done \
        && cd "${LITERT_LM_SRC_DIR}" \
        && git lfs pull \
        && mkdir -p ${QAIRT_DIR} \
        && wget -t 3 -T 120 --no-verbose \
            -P ${QAIRT_DIR} \
            "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip" \
        && cd ${QAIRT_DIR} \
        && unzip v${QAIRT_VERSION}.zip \
        && rm -f v${QAIRT_VERSION}.zip \
        && ln -sf ${QAIRT_DIR}/qairt/${QAIRT_VERSION} \
                  ${QAIRT_DIR}/qairt/2.44.0.260225 \
        && echo "QAIRT ${QAIRT_VERSION} ready; symlink 2.44.0.260225 → ${QAIRT_VERSION}"; \
    fi

# Add cc_shared_library target and build LiteRT-LM (only if BUILD_LITERT_LM=true)
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        export LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm && \
        export LITERT_QAIRT_SDK=/tmp/qairt/ && \
        export LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr && \
        cd ${LITERT_LM_SRC_DIR} \
        && printf '\n# Shared library target for the LiteRT-LM C API.\ncc_shared_library(\n    name = "litert_lm_c_api",\n    exports_filter = ["//c:engine"],\n    deps = [":engine"],\n    user_link_flags = ["-Wl,--export-dynamic"],\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD \
        && bazel build \
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
            //c:litert_lm_c_api \
        && mkdir -p "${LITERT_LM_DEPLOY_DIR}/lib" \
                     "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c" \
                     "${LITERT_LM_DEPLOY_DIR}/include/litert" \
                     "${LITERT_LM_DEPLOY_DIR}/include/tflite" \
        && find -L bazel-bin bazel-out \( -type f -o -type l \) \
            \( -name "liblitert_lm_c_api.so" -o -name "liblitert_lm_c_api.so.*" \
               -o -name "litert_lm_c_api.so" -o -name "litert_lm_c_api.so.*" \
               -o -name "libGemmaModelConstraintProvider.so" \
               -o -name "libllguidance*.so" -o -name "libllguidance*.so.*" \
               -o -name "liblitert_lm_llguidance*.so" -o -name "liblitert_lm_llguidance*.so.*" \) \
            2>/dev/null | while IFS= read -r f; do \
                base=$(basename "$f"); \
                if echo "$base" | grep -q "^lib"; then \
                    cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/" || true; \
                else \
                    cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/lib${base}" || true; \
                fi; \
            done \
        && if [ -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" ]; then \
               readelf -d "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
                   | grep NEEDED | sed 's/.*\[//;s/\]//' \
                   | while IFS= read -r needed_lib; do \
                       if [ ! -f "${LITERT_LM_DEPLOY_DIR}/lib/$needed_lib" ]; then \
                           found=$(find -L bazel-bin bazel-out -name "$needed_lib" 2>/dev/null | head -1); \
                           if [ -n "$found" ]; then \
                               cp -avL "$found" "${LITERT_LM_DEPLOY_DIR}/lib/"; \
                           fi; \
                       fi; \
                   done; \
           fi \
        && cp c/engine.h "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c/" \
        && LITERT_EXTERNAL=$(find bazel-* -type d -path "*/external/litert" 2>/dev/null | head -1) \
        && if [ -n "$LITERT_EXTERNAL" ] && [ -d "$LITERT_EXTERNAL" ]; then \
               find "$LITERT_EXTERNAL" -type f \( -name "*.so" -o -name "*.so.*" \) \
                   -exec cp -avL {} "${LITERT_LM_DEPLOY_DIR}/lib/" \; 2>/dev/null || true; \
               if [ -d "$LITERT_EXTERNAL/litert" ]; then \
                   cp -a "$LITERT_EXTERNAL/litert" "${LITERT_LM_DEPLOY_DIR}/include/" || true; \
               fi; \
               if [ -d "$LITERT_EXTERNAL/tflite" ]; then \
                   cp -a "$LITERT_EXTERNAL/tflite" "${LITERT_LM_DEPLOY_DIR}/include/" || true; \
               fi; \
           fi; \
    fi

# Build Rust cdylib wrappers (only if BUILD_LITERT_LM=true)
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        export LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr && \
        wget -q -O /tmp/rustup.sh https://raw.githubusercontent.com/rust-lang/rustup/main/rustup-init.sh \
        && sh /tmp/rustup.sh -y --default-toolchain stable --profile minimal \
        && . /root/.cargo/env \
        && LLGUIDANCE_SRC=$(find /root/.cache/bazel -path "*/crate_index__llguidance-1.3.0" -type d 2>/dev/null | head -1) \
        && if [ -n "$LLGUIDANCE_SRC" ]; then \
               mkdir -p /tmp/llg-build && cd /tmp/llg-build \
               && printf '[package]\nname = "llguidance-cdylib"\nversion = "1.3.0"\nedition = "2021"\n\n[lib]\ncrate-type = ["cdylib"]\n\n[dependencies]\nllguidance = { path = "%s", default-features = false, features = ["ahash", "lark", "rayon"] }\n' "$LLGUIDANCE_SRC" > Cargo.toml \
               && mkdir -p src && echo 'pub use llguidance::*;' > src/lib.rs \
               && cargo build --release \
               && CAPI_SO=$(find target/release -name "libllguidance_cdylib.so" -type f 2>/dev/null | head -1) \
               && if [ -n "$CAPI_SO" ]; then \
                      cp -avL "$CAPI_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libllguidance.so"; \
                  fi \
               && rm -rf /tmp/llg-build; \
           fi \
        && TOKENIZERS_CPP_SRC=$(find /root/.cache/bazel -path "*/external/tokenizers_cpp" -type d 2>/dev/null | head -1) \
        && TOKENIZERS_EXACT_SRC=$(find /root/.cache/bazel -path "*/crate_index__tokenizers-0.21.0" -type d 2>/dev/null | head -1) \
        && if [ -n "$TOKENIZERS_CPP_SRC" ] && [ -n "$TOKENIZERS_EXACT_SRC" ] && [ -d "$TOKENIZERS_CPP_SRC/rust" ]; then \
               cd "$TOKENIZERS_CPP_SRC/rust" \
               && python3 -c "import re; content=open('Cargo.toml').read(); patched=re.sub(r'crate-type\s*=\s*\[.*?\]', 'crate-type = [\"cdylib\"]', content); patched=patched if patched!=content else (content+'\n[lib]\ncrate-type = [\"cdylib\"]\n' if '[lib]' not in content else content); open('Cargo.toml','w').write(patched)" \
               && printf '\n[patch.crates-io]\ntokenizers = { path = "%s" }\n' "$TOKENIZERS_EXACT_SRC" >> Cargo.toml \
               && sed -i 's/\*out_len = (\*handle)\.decode_str\.len();/*out_len = (\&(*handle).decode_str).len();/' src/lib.rs \
               && sed -i 's/\*out_len = (\*handle)\.id_to_token_result\.len();/*out_len = (\&(*handle).id_to_token_result).len();/' src/lib.rs \
               && cargo build --release \
               && TOK_SO=$(find target/release -name "libtokenizers_c.so" -type f 2>/dev/null | head -1) \
               && if [ -n "$TOK_SO" ]; then \
                      cp -avL "$TOK_SO" "${LITERT_LM_DEPLOY_DIR}/lib/libtokenizers_c.so"; \
                  fi; \
           fi \
        && for so in "${LITERT_LM_DEPLOY_DIR}/lib/"*.so; do \
               [ -f "$so" ] && patchelf --set-rpath '$ORIGIN' "$so" 2>/dev/null || true; \
           done; \
    fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: llama.cpp Builder (conditional via shell if)
#
# Builds llama.cpp with Hexagon NPU backend when BUILD_LLAMACPP=true.
# When BUILD_LLAMACPP=false, only creates minimal directory structure (zero build cost).
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS llamacpp_builder

# Re-declare ARG after FROM
ARG BUILD_LLAMACPP=false

# Always create minimal directory structure
RUN mkdir -p /install/bin /install/lib /install/lib/rfsa/adsp

# Conditional build: Only execute expensive operations when BUILD_LLAMACPP=true
RUN if [ "$BUILD_LLAMACPP" = "true" ]; then \
        echo "=== Building llama.cpp (BUILD_LLAMACPP=true) ===" && \
        apt-get update && \
        apt-get install -y --no-install-recommends \
            build-essential \
            cmake \
            ninja-build \
            pkg-config \
            git \
            wget \
            unzip \
            ca-certificates \
            autoconf \
            automake \
            libtool \
            m4 \
            libbsd-dev \
            libmd-dev \
            libyaml-dev \
            binutils \
            python3 \
            python3-pip \
            python-is-python3 \
            qemu-user-static \
        && apt-get clean \
        && rm -rf /var/lib/apt/lists/* \
        && pip3 install cmake --upgrade; \
    else \
        echo "=== Skipping llama.cpp build (BUILD_LLAMACPP=false) ==="; \
    fi

# Build FastRPC, download Hexagon SDK, and build llama.cpp (only if BUILD_LLAMACPP=true)
RUN if [ "$BUILD_LLAMACPP" = "true" ]; then \
        git clone --depth 1 https://github.com/quic/fastrpc.git /tmp/fastrpc \
        && cd /tmp/fastrpc \
        && autoreconf -is \
        && CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
            ./configure --prefix=/usr --libdir=/usr/lib \
        && make -j$(nproc) \
        && make install \
        && find /tmp/fastrpc -name 'remote.h' -exec cp {} /usr/include/ \; \
        && rm -rf /tmp/fastrpc \
        && wget --no-check-certificate \
            https://softwarecenter.qualcomm.com/api/download/software/sdks/Hexagon_SDK/Linux/Debian/6.6.0.0/Hexagon_SDK_Linux.zip \
            -O /tmp/hexagon_sdk.zip \
        && unzip -q /tmp/hexagon_sdk.zip -d /opt \
        && rm /tmp/hexagon_sdk.zip \
        && wget --no-check-certificate \
            https://softwarecenter.qualcomm.com/api/download/software/tools/Hexagon_open_access/Linux/ARM64/Debian/19.0.02/Hexagon_open_access.Core.19.0.02.Linux-ARM64.tar.gz \
            -O /tmp/hexagon_tools.tar.gz \
        && mkdir -p /opt/hexagon \
        && tar -xzf /tmp/hexagon_tools.tar.gz -C /opt/hexagon/ \
        && rm /tmp/hexagon_tools.tar.gz \
        && export HEXAGON_SDK_ROOT=/opt/Hexagon_SDK/6.6.0.0 \
        && export HEXAGON_CMAKE_ROOT=/opt/Hexagon_SDK/6.6.0.0/build/cmake \
        && export HEXAGON_TOOLS_ROOT=/opt/hexagon \
        && export PATH=/opt/hexagon/Tools/bin:${PATH} \
        && cp /usr/include/remote.h ${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/UbuntuARM_aarch64/ \
        && cp /usr/include/remote.h ${HEXAGON_SDK_ROOT}/ipc/fastrpc/remote/ship/android_aarch64/ 2>/dev/null || true \
        && sed -i 's/string(FIND \${PREBUILT_LIB_DIR} /string(FIND "${PREBUILT_LIB_DIR}" /g' \
            ${HEXAGON_SDK_ROOT}/build/cmake/hexagon_fun.cmake \
        && dpkg --add-architecture amd64 \
        && echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy main restricted universe multiverse" > /etc/apt/sources.list.d/amd64.list \
        && echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-updates main restricted universe multiverse" >> /etc/apt/sources.list.d/amd64.list \
        && echo "deb [arch=amd64] http://archive.ubuntu.com/ubuntu jammy-security main restricted universe multiverse" >> /etc/apt/sources.list.d/amd64.list \
        && sed -i 's/^deb /deb [arch=arm64] /' /etc/apt/sources.list \
        && apt-get update \
        && apt-get install -y --no-install-recommends \
            libc6:amd64 \
            libstdc++6:amd64 \
            libgmp10:amd64 \
        && apt-get clean && rm -rf /var/lib/apt/lists/* \
        && mkdir -p ${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin \
        && printf '#!/bin/sh\nexec qemu-x86_64-static %s "$@"\n' \
            "${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/Ubuntu/qaic" \
            > ${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin/qaic \
        && chmod +x ${HEXAGON_SDK_ROOT}/ipc/fastrpc/qaic/bin/qaic \
        && mkdir -p /build \
        && cd /build \
        && git clone --depth 1 https://github.com/ggerganov/llama.cpp.git \
        && cd llama.cpp \
        && sed -i 's/GGML_SCHED_MAX_SPLIT_INPUTS [0-9]*/GGML_SCHED_MAX_SPLIT_INPUTS 256/' ggml/src/ggml-backend.cpp \
        && cmake -S . -B build \
            -DCMAKE_BUILD_TYPE=Release \
            -DGGML_HEXAGON=ON \
            -DGGML_NATIVE=OFF \
            -DGGML_BACKEND_DL=OFF \
            -DBUILD_SHARED_LIBS=ON \
            -DLLAMA_BUILD_TESTS=OFF \
            -DLLAMA_BUILD_EXAMPLES=OFF \
            -DLLAMA_BUILD_SERVER=ON \
            -DHEXAGON_SDK_ROOT=${HEXAGON_SDK_ROOT} \
            -DHEXAGON_TOOLS_ROOT=${HEXAGON_TOOLS_ROOT} \
            -DHEXAGON_CMAKE_ROOT=${HEXAGON_CMAKE_ROOT} \
            -DHEXAGON_ARCH=v73 \
            -DPREBUILT_LIB_DIR=linux_aarch64 \
            -DCMAKE_INSTALL_PREFIX=/install \
            -GNinja \
        && cmake --build build -j4 \
        && echo "=== Verifying build artifacts ===" \
        && find build -name "*.so" -type f \
        && find build -name "llama-server" -type f \
        && cmake --install build \
        && echo "=== Verifying installation ===" \
        && ls -la /install/lib/ \
        && ls -la /install/bin/ \
        && if [ -z "$(ls -A /install/lib/*.so 2>/dev/null)" ]; then \
               echo "WARNING: Libraries not installed, copying manually..."; \
               find build -name "*.so" -type f -exec cp -v {} /install/lib/ \; ; \
           fi \
        && if [ ! -f "/install/bin/llama-server" ]; then \
               echo "WARNING: llama-server not installed, copying manually..."; \
               find build -name "llama-server" -type f -exec cp -v {} /install/bin/ \; ; \
           fi \
        && find /install/bin -type f -exec strip --strip-unneeded {} \; 2>/dev/null || true \
        && mkdir -p /install/lib/rfsa/adsp \
        && find /install/lib -maxdepth 1 -name 'libggml-htp-v*.so' \
            -exec cp {} /install/lib/rfsa/adsp/ \; 2>/dev/null || true; \
    fi

# Collect llama.cpp headers into /install/include for use by qai-forge build
RUN if [ "$BUILD_LLAMACPP" = "true" ]; then \
        mkdir -p /install/include/ggml/include \
        && cp -r /build/llama.cpp/ggml/include/* /install/include/ggml/include/ \
        && cp -r /build/llama.cpp/include/* /install/include/ 2>/dev/null || true; \
    else \
        mkdir -p /install/include; \
    fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS builder

# Install build dependencies
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

ENV DEPLOY_DIR=/build/deploy

# Build fastrpc from source
RUN git clone --depth 1 https://github.com/quic/fastrpc.git /tmp/fastrpc && \
    cd /tmp/fastrpc && \
    autoreconf -is && \
    CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"' \
        ./configure --prefix=/usr --libdir=/usr/lib && \
    make -j$(nproc) && \
    make install && \
    rm -rf /tmp/fastrpc

# Build Drogon from source
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

# Fetch and install QAIRT SDK
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

# Copy fastrpc runtime libraries
RUN cp /usr/lib/libcdsprpc.so* ${DEPLOY_DIR}/usr/lib/ && \
    cp /usr/lib/libadsprpc.so* ${DEPLOY_DIR}/usr/lib/

# Copy LiteRT-LM artifacts
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib /usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib ${DEPLOY_DIR}/usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/include /usr/include

# Copy llama.cpp artifacts
COPY --from=llamacpp_builder /install/bin ${DEPLOY_DIR}/usr/bin
COPY --from=llamacpp_builder /install/lib /usr/lib
COPY --from=llamacpp_builder /install/lib ${DEPLOY_DIR}/usr/lib
COPY --from=llamacpp_builder /install/lib/rfsa/adsp ${DEPLOY_DIR}/usr/lib/rfsa/adsp
COPY --from=llamacpp_builder /install/include /usr/include

# Diagnostic: locate gguf.h to verify include path structure
RUN echo "=== [DIAGNOSTIC] Searching for gguf.h under /usr/include ===" \
    && find /usr/include -name "gguf.h" 2>/dev/null \
    && echo "=== [DIAGNOSTIC] ggml-related files under /usr/include ===" \
    && find /usr/include -name "ggml*" -o -name "gguf*" -o -name "llama*" 2>/dev/null | head -20 \
    && echo "=== [DIAGNOSTIC] /usr/include/ggml directory (if exists) ===" \
    && (ls -laR /usr/include/ggml/ 2>/dev/null || echo "(no /usr/include/ggml/ directory)") \
    && echo "=== [DIAGNOSTIC] Top-level /usr/include contents ===" \
    && ls /usr/include/ | grep -E "(ggml|gguf|llama)" 2>/dev/null || echo "(no llama.cpp headers at top level)" \
    && echo "=== [DIAGNOSTIC] done ==="

# Build qai-forge
WORKDIR /build
COPY qai-forge /build/qai-forge

ARG BUILD_LITERT_LM
ARG BUILD_LLAMACPP
RUN cmake -S /build/qai-forge -B /build/qai-forge/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENIE_INCLUDE_PATH=/usr/include \
        -DGENIE_LIB_PATH=/usr/lib \
        -DQAI_FORGE_BUILD_LITERT_LM=${BUILD_LITERT_LM} \
        -DLITERT_LM_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_LLAMACPP=${BUILD_LLAMACPP} \
        -DLLAMACPP_INCLUDE_PATH=/usr/include \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/qai-forge/build -j$(nproc) && \
    cmake --install /build/qai-forge/build --prefix ${DEPLOY_DIR}/usr

# Build responses server
COPY responses /build/responses

RUN cmake -S /build/responses -B /build/responses/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/responses/build -j$(nproc) && \
    cmake --install /build/responses/build

# Verify LiteRT-LM dependencies
RUN echo "Verifying LiteRT-LM library dependencies:" \
    && export LD_LIBRARY_PATH=${DEPLOY_DIR}/usr/lib:/usr/lib:$LD_LIBRARY_PATH \
    && if [ -f "${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so" ]; then \
           ldd ${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so || { \
               echo "ERROR: liblitert_lm_c_api.so has unresolved dependencies"; \
               exit 1; \
           }; \
       fi \
    && echo "✓ LiteRT-LM dependencies verified"

# Strip binaries
RUN find ${DEPLOY_DIR}/usr/bin -type f \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage 3: Stager
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS stager

# Install runtime deps
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
        libgomp1 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Assemble /export tree
RUN mkdir -p /export/usr/bin \
             /export/usr/lib/rfsa/adsp \
             /export/etc/fastrpc

COPY --from=builder /build/deploy/usr/bin /export/usr/bin
COPY --from=builder /build/deploy/usr/lib /export/usr/lib

# Cherry-pick system libraries
RUN for lib in libyaml-0 libbsd libmd libjsoncpp libssl libcrypto libz libuuid libatomic libgomp; do \
      find /usr/lib /lib -name "${lib}.so*" -exec cp -dp {} /export/usr/lib/ \; ; \
    done

# Create fastrpc config
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
# Stage 4: Runtime (distroless)
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 gcr.io/distroless/cc-debian12 AS responses-runtime

COPY --from=stager /export /

# Set environment defaults
ENV RESPONSES_PORT=9002
ENV GENAI_MODELS_DIR=/mnt/work/models
ENV GENAI_WORKER_BINARY=/usr/bin/genai-inference-worker
ENV GENAI_VLM_WORKER_BINARY=/usr/bin/genai-vlm-inference-worker
ENV LITERT_LM_WORKER_BINARY=/usr/bin/litert-lm-inference-worker
ENV LITERT_LM_BACKEND=npu
ENV LOG_LEVEL=ERROR
ENV GENAI_WORKER_IDLE_TIMEOUT=120
ENV GENAI_WORKER_ACTIVE_TIMEOUT=600

EXPOSE 9002

WORKDIR /mnt/work/models

ENTRYPOINT ["/usr/bin/responses-server"]

ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
