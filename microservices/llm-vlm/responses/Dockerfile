# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# Build arguments for modular backend configuration
# ─────────────────────────────────────────────────────────────────────────────
ARG BUILD_LITERT_LM=true
ARG BUILD_LLAMACPP=false
ARG QAIRT_SDK_VERSION=2.48.0.260626
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
# Uses modular scripts for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS litert_lm_builder

# Re-declare ARG after FROM (required for ARG to be available in this stage)
ARG BUILD_LITERT_LM=false
ARG QAIRT_SDK_VERSION

# Always create minimal directory structure (required for COPY --from= to succeed)
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr
RUN mkdir -p ${LITERT_LM_DEPLOY_DIR}/lib ${LITERT_LM_DEPLOY_DIR}/include ${LITERT_LM_DEPLOY_DIR}/bin

# Conditional build using modular scripts when BUILD_LITERT_LM=true
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

# Build LiteRT-LM and Rust wrappers using modular scripts (only if BUILD_LITERT_LM=true)
ENV LITERT_LM_VERSION=v0.14.0
ENV QAIRT_VERSION=${QAIRT_SDK_VERSION}
ENV LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
ENV QAIRT_DIR=/tmp/qairt
ENV BAZELISK_VERSION=v1.28.1
COPY qai-forge/scripts/deps/build-litert-lm-standalone.sh /tmp/
COPY qai-forge/scripts/deps/build-rust-wrappers.sh /tmp/
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        chmod +x /tmp/build-litert-lm-standalone.sh && \
        /tmp/build-litert-lm-standalone.sh && \
        rm /tmp/build-litert-lm-standalone.sh && \
        chmod +x /tmp/build-rust-wrappers.sh && \
        /tmp/build-rust-wrappers.sh && \
        rm /tmp/build-rust-wrappers.sh; \
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

ENV LLAMACPP_DEPLOY_DIR=/mnt/work/deploy/usr
RUN mkdir -p ${LLAMACPP_DEPLOY_DIR}/bin ${LLAMACPP_DEPLOY_DIR}/lib ${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp ${LLAMACPP_DEPLOY_DIR}/include

# Conditional build using modular script
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

# Build llama.cpp using modular script (only if BUILD_LLAMACPP=true)
ENV LLAMACPP_VERSION=master
ENV HEXAGON_SDK_VERSION=6.6.0.0
ENV HEXAGON_TOOLS_VERSION=19.0.02
ENV HEXAGON_ARCH=v73
COPY qai-forge/scripts/deps/build-llamacpp-hexagon.sh /tmp/
RUN if [ "$BUILD_LLAMACPP" = "true" ]; then \
        chmod +x /tmp/build-llamacpp-hexagon.sh && \
        /tmp/build-llamacpp-hexagon.sh && \
        rm /tmp/build-llamacpp-hexagon.sh; \
    fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS builder

ARG QAIRT_SDK_VERSION

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

# Build fastrpc from source using modular script
COPY qai-forge/scripts/deps/build-fastrpc.sh /tmp/
RUN chmod +x /tmp/build-fastrpc.sh && /tmp/build-fastrpc.sh && rm /tmp/build-fastrpc.sh

# Build Drogon from source using modular script
ENV DROGON_VERSION=v1.9.6
COPY responses/scripts/build-drogon.sh /tmp/
RUN chmod +x /tmp/build-drogon.sh && /tmp/build-drogon.sh && rm /tmp/build-drogon.sh

# Fetch and install QAIRT SDK using modular script
ENV QAIRT_VERSION=${QAIRT_SDK_VERSION}
ENV QAIRT_DOWNLOAD_DIR=/tmp/qairt-download
COPY qai-forge/scripts/deps/fetch-qairt-sdk.sh /tmp/
RUN chmod +x /tmp/fetch-qairt-sdk.sh && /tmp/fetch-qairt-sdk.sh && rm /tmp/fetch-qairt-sdk.sh

# Remove unnecessary libraries from deploy directory
RUN rm -f \
    "${DEPLOY_DIR}/usr/lib/libQnnGpu.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnGpuNetRunExtensions.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnGpuProfilingReader.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV68CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV69CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV69Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV73CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV75CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV79CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV79Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV81CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnHtpV81Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnLpai.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnLpaiNetRunExtensions.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnLpaiProfilingReader.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnLpaiStub.so" \
    "${DEPLOY_DIR}/usr/lib/libQnnTFLiteDelegate.so" \
    "${DEPLOY_DIR}/usr/lib/libSNPE.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeDspV66Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHta.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpPrepare.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV68CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV68Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV73CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV73Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV75CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV75Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV79CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV79Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV81CalculatorStub.so" \
    "${DEPLOY_DIR}/usr/lib/libSnpeHtpV81Stub.so" \
    "${DEPLOY_DIR}/usr/lib/libcalculator.so" \
    "${DEPLOY_DIR}/usr/lib/libhta_hexagon_runtime_snpe.so"

# Copy LiteRT-LM artifacts
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/lib /usr/lib
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/lib ${DEPLOY_DIR}/usr/lib
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/include /usr/include

# Copy llama.cpp artifacts
ENV LLAMACPP_DEPLOY_DIR=/mnt/work/deploy/usr
COPY --from=llamacpp_builder ${LLAMACPP_DEPLOY_DIR}/bin ${DEPLOY_DIR}/usr/bin
COPY --from=llamacpp_builder ${LLAMACPP_DEPLOY_DIR}/lib /usr/lib
COPY --from=llamacpp_builder ${LLAMACPP_DEPLOY_DIR}/lib ${DEPLOY_DIR}/usr/lib
COPY --from=llamacpp_builder ${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp ${DEPLOY_DIR}/usr/lib/rfsa/adsp
COPY --from=llamacpp_builder ${LLAMACPP_DEPLOY_DIR}/include /usr/include

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

ENV DEPLOY_DIR=/build/deploy

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

COPY --from=builder ${DEPLOY_DIR}/usr/bin /export/usr/bin
COPY --from=builder ${DEPLOY_DIR}/usr/lib /export/usr/lib

# Cherry-pick system libraries using modular script
ENV EXPORT_DIR=/export
COPY responses/scripts/cherry-pick-system-libs.sh /tmp/
RUN chmod +x /tmp/cherry-pick-system-libs.sh && /tmp/cherry-pick-system-libs.sh && rm /tmp/cherry-pick-system-libs.sh

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
