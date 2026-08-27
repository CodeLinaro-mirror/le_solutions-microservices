# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# QAIServe/Dockerfile
#
# Builds the QAIServe unified inference server container.
#
# Build context: llm-vlm/  (parent directory containing all siblings)
# Usage (from llm-vlm/):
#   docker build -f QAIServe/Dockerfile -t qaiserve-server .
#
# To build against a different QAIRT SDK version:
#   docker build -f QAIServe/Dockerfile --build-arg QAIRT_SDK_VERSION=2.46.0.260500 -t qaiserve-server .
#
# The build context must include both:
#   - qai-forge/  (shared SDK: Layers 2-4 + worker binaries + daemon)
#   - QAIServe/ (Layer 1: Drogon HTTP server for Responses & KFServe API)
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
# Three-stage build for minimal image size:
#   Stage 1 (builder):  Compile everything; strip our own ELF artifacts.
#   Stage 2 (stager):   Cherry-pick only the required system .so files from
#                       a full Debian environment; assemble /export tree.
#   Stage 3 (runtime):  debian:trixie-slim — COPY /export in.
#                       Minimal runtime; no build tools, no apt cache.
#
#   Privilege drop: handled natively by the C++ server binary at startup.
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# Build arguments (override with --build-arg at `docker build` time)
#
#   QAIRT_SDK_VERSION  Qualcomm AI Runtime Community SDK version. Used both
#                       for the LiteRT QNN dispatch/compiler-plugin build
#                       (Stage 0) and for the QAIServe application build
#                       (Stage 1). Defaults to the version validated for
#                       this Dockerfile.
#
#   LITERT_VERSION      LiteRT release tag to build in Stage 0 (litert_builder).
#                       Defaults to the version validated for this Dockerfile.
#
#   LITERT_LM_VERSION   LiteRT-LM release tag to build in Stage 0b
#                       (litert_lm_builder). Defaults to the version validated
#                       for this Dockerfile.
#
#   BUILD_LITERT_LM_CLI_TOOLS  Build the litert-lm / litert-lm-advanced
#                       on-device debugging/benchmarking CLI binaries
#                       (Stage 0b). QAIServe never calls these at runtime —
#                       it drives LiteRT-LM inference via
#                       liblitert_lm_c_api.so through litert-lm-inference-worker.
#                       Left off by default to keep the image small; pass
#                       --build-arg BUILD_LITERT_LM_CLI_TOOLS=true to build
#                       and ship them again.
#
# ─────────────────────────────────────────────────────────────────────────────
ARG QAIRT_SDK_VERSION=2.48.0.260626
ARG LITERT_VERSION=2.1.6
ARG LITERT_LM_VERSION=0.14.0
ARG BUILD_LITERT_LM_CLI_TOOLS=false

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0: litert_builder
#
# Builds LiteRT natively on AArch64 using Bazelisk.
# Uses modular script for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS litert_builder

# Re-declare to bring the global ARGs into this stage's scope.
ARG QAIRT_SDK_VERSION
ARG LITERT_VERSION

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
        patchelf \
        default-jdk-headless \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Use ARM64 Bazelisk for native AArch64 build
ENV BAZELISK_VERSION=v1.28.1
RUN wget -t 3 -T 120 --no-verbose \
        -O /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64" \
    && chmod +x /usr/local/bin/bazel

# Build LiteRT using modular script
ENV LITERT_VERSION=${LITERT_VERSION}
ENV QAIRT_LITERT_VERSION=${QAIRT_SDK_VERSION}
ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
ENV QAIRT_LITERT_DIR=/tmp/qairt-litert
ENV HERMETIC_PYTHON_VERSION=3.10
ENV PYTHON_BIN_PATH=/usr/bin/python3
ENV PYTHON_LIB_PATH=/usr/lib/python3/dist-packages
ENV TF_NEED_CUDA=0
ENV TF_NEED_ROCM=0
ENV TF_NEED_TENSORRT=0
ENV TF_NEED_OPENCL_SYCL=0
ENV TF_NEED_OPENCL=0
ENV TF_NEED_MPI=0
ENV TF_NEED_COMPUTECPP=0
ENV TF_NEED_CLANG=1
ENV CLANG_COMPILER_PATH=/usr/bin/clang
ENV TF_SET_ANDROID_WORKSPACE=0
ENV TF_DOWNLOAD_CLANG=0
ENV TF_ENABLE_XLA=0

COPY qai-forge/scripts/deps/build-litert.sh /tmp/
RUN chmod +x /tmp/build-litert.sh && /tmp/build-litert.sh && rm /tmp/build-litert.sh

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0b: litert_lm_builder
#
# Builds LiteRT-LM incrementally on top of the already-built LiteRT.
# Uses modular script for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
FROM litert_builder AS litert_lm_builder

# Re-declare to bring the global ARGs into this stage's scope, then export as
# ENV so build-litert-lm-incremental.sh (a plain RUN, not a build-arg-aware
# step) can read them.
ARG BUILD_LITERT_LM_CLI_TOOLS
ARG LITERT_LM_VERSION
ENV BUILD_LITERT_LM_CLI_TOOLS=${BUILD_LITERT_LM_CLI_TOOLS}
ENV LITERT_LM_VERSION=${LITERT_LM_VERSION}

# Build LiteRT-LM using modular script
# LITERT_SRC_DIR is already inherited from litert_builder (this stage is
# FROM litert_builder); re-declared here so the stage's inputs are explicit.
ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr

COPY qai-forge/scripts/deps/build-litert-lm-incremental.sh /tmp/
COPY qai-forge/scripts/deps/build-rust-wrappers.sh /tmp/
RUN chmod +x /tmp/build-litert-lm-incremental.sh /tmp/build-rust-wrappers.sh && \
    /tmp/build-litert-lm-incremental.sh && \
    rm /tmp/build-litert-lm-incremental.sh /tmp/build-rust-wrappers.sh

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 debian:trixie-slim AS builder

# Re-declare to bring the global ARG into this stage's scope.
ARG QAIRT_SDK_VERSION

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
        protobuf-compiler \
        libprotobuf-dev \
        libgrpc++-dev \
        protobuf-compiler-grpc \
        libc-ares-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# ── Deploy directory (all runtime artifacts staged here) ─────────────────────
# Defined early so all subsequent build steps can install directly into it.
ENV DEPLOY_DIR=/build/deploy

# Re-declared here (same literal values as Stage 0/0b) so the COPY --from=
# lines below can reference them — ENV does not cross stage boundaries, and
# this stage is FROM debian:trixie-slim, not FROM litert_builder.
ENV LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr

# ── Build fastrpc from source ─────────────────────────────────────────────────
# Uses modular script for maintainability and reusability
COPY qai-forge/scripts/deps/build-fastrpc.sh /tmp/
RUN chmod +x /tmp/build-fastrpc.sh && /tmp/build-fastrpc.sh && rm /tmp/build-fastrpc.sh

# ── Build lightweight Drogon from source ──────────────────────────────────────
# Uses modular script for maintainability and reusability
COPY QAIServe/scripts/build-drogon.sh /tmp/
RUN chmod +x /tmp/build-drogon.sh && /tmp/build-drogon.sh && rm /tmp/build-drogon.sh

# ── Fetch and install Qualcomm AI Runtime Community (QAIRT) SDK ───────────────
# Uses modular script for maintainability and reusability
# Note: QAIServe keeps QNN and SNPE libraries (unlike responses server)
# fetch-qairt-sdk.sh copies QNN/SNPE/Genie headers to /usr/include (needed to
# compile qai-forge's qnn-engine/snpe-engine below) and stages the aarch64
# .so files (dlopen'd at runtime) into ${DEPLOY_DIR}/usr/lib — all before it
# deletes its own download scratch dir. Nothing further to copy here.
ENV QAIRT_VERSION=${QAIRT_SDK_VERSION}
ENV QAIRT_DOWNLOAD_DIR=/tmp/qairt-download
COPY qai-forge/scripts/deps/fetch-qairt-sdk.sh /tmp/
RUN chmod +x /tmp/fetch-qairt-sdk.sh && /tmp/fetch-qairt-sdk.sh && rm /tmp/fetch-qairt-sdk.sh

# ── Stage the GenieX SDK (libgeniex.so + headers) ────────────────────────────
# Uses modular script for maintainability and reusability
ENV GENIEX_SDK_VERSION=v0.3.14
COPY QAIServe/scripts/fetch-geniex-sdk.sh /tmp/
RUN chmod +x /tmp/fetch-geniex-sdk.sh && /tmp/fetch-geniex-sdk.sh && rm /tmp/fetch-geniex-sdk.sh

# ── Copy LiteRT libraries built from source (AArch64, with QNN NPU dispatch) ──
# Overlays on top of the QAIRT libraries so the source-built LiteRT runtime
# (liblitert_runtime_c_api.so) and dispatch/compiler-plugin .so files are
# available for both build-time linking and runtime loading.
# Headers land in /usr/include/litert/ and /usr/include/tflite/.
COPY --from=litert_builder ${LITERT_DEPLOY_DIR} /usr
COPY --from=litert_builder ${LITERT_DEPLOY_DIR}/lib ${DEPLOY_DIR}/usr/lib

# ── Copy LiteRT-LM libraries and CLI tools (built on top of LiteRT) ──────────
# LiteRT-LM was built with --override_repository=litert=${LITERT_SRC_DIR}
# so it uses the exact same LiteRT version as above.
# The .so files land in /usr/lib/ alongside the LiteRT runtime libraries.
# The CLI tools (litert-lm, litert-lm-advanced) land in ${DEPLOY_DIR}/usr/bin/
# for inclusion in the final image as on-device debugging/benchmarking tools.
# By default (BUILD_LITERT_LM_CLI_TOOLS=false) these are stub scripts staged
# by build-litert-lm-incremental.sh rather than the real multi-MB Bazel-built
# binaries — QAIServe itself never calls them. Pass
# --build-arg BUILD_LITERT_LM_CLI_TOOLS=true to build and ship the real tools.
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/lib /usr/lib
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/lib ${DEPLOY_DIR}/usr/lib
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/include /usr/include
COPY --from=litert_lm_builder ${LITERT_LM_DEPLOY_DIR}/bin ${DEPLOY_DIR}/usr/bin

# ── Build llama.cpp (provides llama-server + libllama.so + libggml*.so) ───────
# Uses modular script for maintainability and reusability
ENV LLAMACPP_INSTALL_DIR=/build/llamacpp-install
COPY qai-forge/scripts/deps/build-llamacpp.sh /tmp/
RUN chmod +x /tmp/build-llamacpp.sh && /tmp/build-llamacpp.sh && rm /tmp/build-llamacpp.sh

# ── Build qai-forge (shared SDK: Layers 2-4 + worker binaries) ───────────────
WORKDIR /build
COPY qai-forge /build/qai-forge

RUN cmake -S /build/qai-forge -B /build/qai-forge/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENIE_INCLUDE_PATH=/usr/include \
        -DGENIE_LIB_PATH=/usr/lib \
        -DQNN_INCLUDE_PATH=/usr/include \
        -DSNPE_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_QNN=ON \
        -DQAI_FORGE_BUILD_SNPE=ON \
        -DQAI_FORGE_BUILD_LITERT=ON \
        -DLITERT_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_LITERT_LM=ON \
        -DLITERT_LM_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_LLAMACPP=ON \
        -DLLAMACPP_INCLUDE_PATH=${LLAMACPP_INSTALL_DIR}/include \
        -DLLAMACPP_LIB_PATH=${LLAMACPP_INSTALL_DIR}/lib \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/qai-forge/build -j$(nproc) && \
    cmake --install /build/qai-forge/build --prefix ${DEPLOY_DIR}/usr

# ── Install QAIServe-specific build deps (CURL + libzip) ─────────────────────
# Placed here (just before COPY QAIServe) so that changes to this step do not
# invalidate the expensive LiteRT / LiteRT-LM / qai-forge build cache above.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libcurl4-openssl-dev \
        libzip-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* \
    && touch /usr/bin/zipcmp /usr/bin/zipmerge /usr/bin/ziptool \
    && chmod +x /usr/bin/zipcmp /usr/bin/zipmerge /usr/bin/ziptool

# ── Build QAIServe server (Layer 1: Drogon HTTP server) ──────────────────────
COPY QAIServe /build/QAIServe

RUN cmake -S /build/QAIServe -B /build/QAIServe/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/QAIServe/build -j$(nproc) && \
    cmake --install /build/QAIServe/build

# ── Strip our own compiled ELF artifacts to reduce size ──────────────────────
# Strips debug symbols from:
#   - All executables in bin/ (our server + worker binaries)
#   - Our own compiled .so files (Drogon, trantor, qai_forge, fastrpc)
#   - Built-in postprocess plugin .so files 
# Deliberately skips ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ — those are Hexagon DSP
# blobs compiled for the DSP core, not ARM64; stripping them would corrupt them.
# Vendor QAIRT .so files are already stripped release binaries; strip is a no-op.
RUN find ${DEPLOY_DIR}/usr/bin -type f \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib/postproc_plugins -maxdepth 1 -type f -name "*.so" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: Stager
#
# Installs runtime apt packages solely to obtain their .so files, then
# cherry-picks only the required libraries into /export. Nothing else from
# this stage's OS lands in the final image.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 debian:trixie-slim AS stager

# Same value as the builder stage's DEPLOY_DIR — re-declared here since ENV
# does not cross stage boundaries and this stage is FROM debian:trixie-slim,
# not FROM builder.
ENV DEPLOY_DIR=/build/deploy

# Install runtime deps to make their .so files available for cherry-picking.
# libgrpc++1.51/libgrpc29/libprotobuf32 pull in abseil + re2 + c-ares
# transitively — apt resolves the exact versioned packages; we only
# cherry-pick the .so files below, not the packages themselves.
#
# NOTE: on trixie the runtime gRPC/protobuf packages carry the 64-bit time_t
# suffix — libgrpc++1.51t64 / libgrpc29t64 / libprotobuf32t64 (the plain
# bookworm names libgrpc++1.51 / libgrpc29 / libprotobuf32 do not exist on
# trixie and silently resolve to nothing, causing qaiserve to fail at startup
# with "libgrpc++.so.1.51: cannot open shared object file"). There is no
# separate "libgpr" package; the gpr symbols live inside libgrpc29t64. abseil /
# re2 / c-ares / upb are pulled in transitively by these packages and
# cherry-picked below by SONAME, so they are not listed explicitly here.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libyaml-0-2 \
        libbsd0 \
        libmd0 \
        libjsoncpp26 \
        libssl3 \
        zlib1g \
        libuuid1 \
        libatomic1 \
        libcurl4 \
        libzip5 \
        libnghttp2-14 \
        libnghttp3-9 \
        librtmp1 \
        libidn2-0 \
        libssh2-1 \
        libpsl5 \
        libgssapi-krb5-2 \
        libldap2 \
        libbrotli1 \
        libzstd1 \
        liblzma5 \
        libbz2-1.0 \
        libsasl2-2 \
        libunistring5 \
        libgnutls30 \
        libgrpc++1.51t64 \
        libgrpc29t64 \
        libprotobuf32t64 \
        libgomp1 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Assemble the /export tree that will be COPY'd into the trixie-slim runtime.
RUN mkdir -p /export/usr/bin \
             /export/usr/lib/rfsa/adsp \
             /export/etc/fastrpc \
             /export/etc/ssl/certs

# Stage CA certificates so in-container HTTPS works (AiHubClient libcurl +
# libgeniex both do TLS to S3/HuggingFace). trixie-slim does not ship certs by
# default; without this, all HTTPS fails with "SSL CA cert (path? access rights?)".
RUN cp -a /etc/ssl/certs/. /export/etc/ssl/certs/ && \
    mkdir -p /export/usr/share/ca-certificates && \
    cp -a /usr/share/ca-certificates/. /export/usr/share/ca-certificates/ 2>/dev/null || true

# Copy stripped binaries from the builder deploy tree.
# Deliberately excludes qai-forge-daemon — the HTTP server spawns workers directly.
COPY --from=builder ${DEPLOY_DIR}/usr/bin/qaiserve /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/genai-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/genai-vlm-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/qnn-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/snpe-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/litert-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/litert-lm-inference-worker /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/litert-lm /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/litert-lm-advanced /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/bin/llama-server /export/usr/bin/
COPY --from=builder ${DEPLOY_DIR}/usr/lib /export/usr/lib

# Cherry-pick only the required system .so files (with symlinks preserved).
# Uses modular script for maintainability and reusability
ENV EXPORT_DIR=/export
COPY QAIServe/scripts/cherry-pick-system-libs.sh /tmp/
RUN chmod +x /tmp/cherry-pick-system-libs.sh && /tmp/cherry-pick-system-libs.sh && rm /tmp/cherry-pick-system-libs.sh

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
    } > ${EXPORT_DIR}/etc/fastrpc/hexagon-dsp-binaries.yaml

# ─────────────────────────────────────────────────────────────────────────────
# Stage 3: Runtime (distroless/cc-debian13)
#
# distroless/cc-debian13 is built on Debian Trixie, so it ships the same
# glibc 2.41 + GCC 14 libstdc++ as the debian:trixie-slim builder/stager
# stages above — the prebuilt GenieX SDK (libgeniex.so) requires
# GLIBC_2.39 + GLIBCXX_3.4.32, which distroless-12 (bookworm, glibc 2.36)
# could not provide, but distroless-13 now can. Using the Trixie-based
# distroless image instead of debian:trixie-slim keeps glibc consistent
# across every stage while trimming the shell/package-manager/apt-cache
# that trixie-slim still carries; the stager's cherry-picked .so files
# overlay on top exactly as before.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 gcr.io/distroless/cc-debian13 AS qaiserve-runtime

# Single COPY brings in:
#   /usr/bin/   - qaiserve, genai-inference-worker, genai-vlm-inference-worker,
#                 qnn-inference-worker, snpe-inference-worker, litert-inference-worker,
#                 litert-lm-inference-worker, litert-lm, litert-lm-advanced
#   /usr/lib/   - libqai_forge.so, libdrogon.so, libtrantor.so,
#                 libGenie.so, QNN, SNPE,
#                 liblitert_runtime_c_api.so, liblitertengine.so,
#                 LiteRT dispatch + compiler plugin .so files,
#                 libcdsprpc.so*, libadsprpc.so*,
#                 cherry-picked system .so files
#   /usr/lib/rfsa/adsp/ - Hexagon DSP blobs (v68/v73/v75)
#   /etc/fastrpc/       - hexagon-dsp-binaries.yaml
COPY --from=stager /export /

# Set environment defaults
ENV QAISERVE_PORT=9002
ENV GENAI_MODELS_DIR=/mnt/work/models
ENV POSTPROC_BUILTIN_PLUGINS_DIR=/usr/lib/postproc_plugins
ENV POSTPROC_PLUGINS_DIR=/mnt/work/plugins
ENV GENAI_WORKER_BINARY=/usr/bin/genai-inference-worker
ENV GENAI_VLM_WORKER_BINARY=/usr/bin/genai-vlm-inference-worker
ENV QNN_WORKER_BINARY=/usr/bin/qnn-inference-worker
ENV SNPE_WORKER_BINARY=/usr/bin/snpe-inference-worker
ENV QNN_BACKEND_LIB=/usr/lib/libQnnHtp.so
ENV QNN_SYS_LIB=/usr/lib/libQnnSystem.so
ENV SNPE_DELEGATE=dsp
ENV LITERT_WORKER_BINARY=/usr/bin/litert-inference-worker
ENV LITERT_DISPATCH_DIR=/usr/lib
ENV LITERT_COMPILER_PLUGIN_DIR=/usr/lib
ENV LITERT_HW_ACCELERATORS=5
ENV LITERT_LM_WORKER_BINARY=/usr/bin/litert-lm-inference-worker
ENV LITERT_LM_BACKEND=npu
ENV LOG_LEVEL=ERROR
ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
# Watchdog timeouts — override in docker-compose.yaml as needed
ENV GENAI_WORKER_IDLE_TIMEOUT=120
ENV GENAI_WORKER_ACTIVE_TIMEOUT=600
ENV QAISERVE_MAX_BODY_SIZE=16777216
ENV GRPC_SERVER_PORT=50051

EXPOSE 9002
EXPOSE 50051

WORKDIR /mnt/work/models

# The C++ server binary is the entrypoint.
# It calls drop_privileges_and_bind_devices() before starting Drogon.
# Child processes (inference workers) inherit the dropped privileges.
ENTRYPOINT ["/usr/bin/qaiserve"]
