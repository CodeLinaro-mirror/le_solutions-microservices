# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# QAIServe/Dockerfile — builds the QAIServe unified inference server image.
#
# Build context: llm-vlm/ (needs qai-forge/ + QAIServe/ siblings)
#   docker build -f QAIServe/Dockerfile -t qaiserve-server .
#
# QAIRT SDK fetched from softwarecenter.qualcomm.com; fastrpc built from
# source (quic/fastrpc) for /run/device-model DSP RPC path; Drogon built
# from source with DB/cache backends disabled (avoids libpq via apt).
#
# Stages: 1 (builder) compile+strip, 2 (stager) cherry-pick runtime .so,
# 3 (runtime) debian:trixie-slim. Privilege drop handled by the C++ binary.
# ─────────────────────────────────────────────────────────────────────────────

# Build-arg defaults preserve historical "build everything" behavior.
# BUILD_LITERT_LM/_LLAMACPP/_QNN/_SNPE/_LITERT=OFF skip the corresponding
# build steps and worker/plugin targets entirely (see qai-forge/CMakeLists.txt
# QAI_FORGE_BUILD_* options, which otherwise default OFF).
ARG QAIRT_SDK_VERSION=2.48.0.260626
ARG LITERT_VERSION=2.1.6
ARG LITERT_LM_VERSION=0.14.0
ARG BUILD_LITERT_LM=ON
ARG BUILD_LITERT_LM_CLI_TOOLS=false
ARG BUILD_LLAMACPP=ON
ARG BUILD_GENIE=true
ARG BUILD_QNN=ON
ARG BUILD_SNPE=ON
ARG BUILD_LITERT=ON
ARG BAZEL_JOBS=4

# Stage -1: qairt_fetcher — downloads/extracts the QAIRT SDK zip once;
# litert_builder and builder both COPY --from= this instead of each
# re-downloading. $BUILDPLATFORM (not arm64): no target-arch code runs here.
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS qairt_fetcher

ARG QAIRT_SDK_VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
        wget \
        unzip \
        ca-certificates \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/qairt-sdk && \
    cd /opt/qairt-sdk && \
    i=1; \
    while [ "$i" -le 10 ]; do \
        wget -c -t 1 -T 120 --no-verbose -O "v${QAIRT_SDK_VERSION}.zip" \
            "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_SDK_VERSION}/v${QAIRT_SDK_VERSION}.zip" && break; \
        echo "Download attempt $i failed, retrying in 15 seconds..."; \
        sleep 15; \
        i=$((i + 1)); \
    done && \
    [ -f "v${QAIRT_SDK_VERSION}.zip" ] || { echo "ERROR: QAIRT SDK download failed after 10 attempts"; exit 1; } && \
    unzip "v${QAIRT_SDK_VERSION}.zip" && \
    rm -f "v${QAIRT_SDK_VERSION}.zip"

# Stage 0: litert_builder — cross-compiles LiteRT to AArch64 natively on
# $BUILDPLATFORM (x86_64) via LiteRT's elinux_aarch64 Bazel config, avoiding
# QEMU-user emulation entirely (a bare FROM with no --platform would inherit
# the final image's arm64 target and run Bazel under QEMU instead).
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS litert_builder

# Re-declare to bring the global ARGs into this stage's scope.
ARG QAIRT_SDK_VERSION
ARG LITERT_VERSION
ARG BAZEL_JOBS
ENV BAZEL_JOBS=${BAZEL_JOBS}

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        clang-15 \
        lld \
        lld-15 \
        llvm \
        g++ \
        crossbuild-essential-arm64 \
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

# Use $BUILDPLATFORM-native Bazelisk — on an x86_64 build host this stage
# cross-compiles to AArch64 via LiteRT's elinux_aarch64 Bazel config (see
# build-litert.sh); on a native arm64 build host (e.g. building directly on
# device) build-litert.sh instead builds LiteRT natively. Either way Bazelisk
# itself must match $BUILDPLATFORM or it can't be exec'd at all.
ENV BAZELISK_VERSION=v1.28.1
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    wget -t 3 -T 120 --no-verbose \
        -O /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-${BUILD_ARCH}" \
    && chmod +x /usr/local/bin/bazel

# Build LiteRT using modular script
ENV LITERT_VERSION=${LITERT_VERSION}
ENV QAIRT_LITERT_VERSION=${QAIRT_SDK_VERSION}
ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
ENV QAIRT_LITERT_DIR=/tmp/qairt-litert
# Persisted as a Dockerfile ENV (not just build-litert.sh's shell export) so it
# also reaches litert_lm_builder's separate bazel invocation below — that
# build runs in a different workspace (LITERT_LM_SRC_DIR) with its own bazel
# output_base, so it can't inherit build-litert.sh's in-process export or its
# bazel external-repo cache. Without this, LiteRT-LM's QNN-enabled build would
# fall back to bazel auto-fetching its own hardcoded QAIRT pin, reintroducing
# the version mismatch against the QAIRT_LITERT_VERSION used elsewhere.
ENV LITERT_QAIRT_SDK=/tmp/qairt-litert/
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

# Reuse the QAIRT SDK already downloaded/extracted by qairt_fetcher instead
# of having build-litert.sh fetch its own copy — see the qairt_fetcher stage
# comment above.
COPY --from=qairt_fetcher /opt/qairt-sdk ${QAIRT_LITERT_DIR}

# Registers local_config_embedded_arm as a Bazel 7 platform-aware toolchain
# (required for --platforms=...:linux_aarch64 resolution; see build-litert.sh).
RUN { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo '# Registers local_config_embedded_arm as a platform-aware toolchain —'; \
      echo '# required for --platforms=...:linux_aarch64 resolution; see build-litert.sh.'; \
      echo 'package(default_visibility = ["//visibility:public"])'; \
      echo ''; \
      echo 'toolchain('; \
      echo '    name = "cc_toolchain_aarch64",'; \
      echo '    exec_compatible_with = ['; \
      echo '        "@platforms//cpu:x86_64",'; \
      echo '        "@platforms//os:linux",'; \
      echo '    ],'; \
      echo '    target_compatible_with = ['; \
      echo '        "@platforms//cpu:aarch64",'; \
      echo '        "@platforms//os:linux",'; \
      echo '    ],'; \
      echo '    toolchain = "@local_config_embedded_arm//:cc-compiler-aarch64",'; \
      echo '    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",'; \
      echo ')'; \
    } > /tmp/litert-cross-compile-toolchains-BUILD

COPY qai-forge/scripts/deps/build-litert.sh /tmp/
RUN chmod +x /tmp/build-litert.sh && /tmp/build-litert.sh && rm /tmp/build-litert.sh /tmp/litert-cross-compile-toolchains-BUILD

# Stage 0b: litert_lm_builder — builds LiteRT-LM on top of the already-built
# LiteRT, only when BUILD_LITERT_LM=ON (otherwise stages an empty tree so
# later COPY --from= still succeeds). Cross-compiles the same way as
# litert_builder (FROM litert_builder, same BUILDPLATFORM/x86_64 env).
FROM litert_builder AS litert_lm_builder

# Re-declare to bring the global ARGs into this stage's scope, then export as
# ENV so build-litert-lm-incremental.sh (a plain RUN, not a build-arg-aware
# step) can read them.
ARG BUILD_LITERT_LM
ARG BUILD_LITERT_LM_CLI_TOOLS
ARG LITERT_LM_VERSION
ARG BAZEL_JOBS
ENV BUILD_LITERT_LM_CLI_TOOLS=${BUILD_LITERT_LM_CLI_TOOLS}
ENV LITERT_LM_VERSION=${LITERT_LM_VERSION}
ENV BAZEL_JOBS=${BAZEL_JOBS}

# Build LiteRT-LM using modular script
# LITERT_SRC_DIR is already inherited from litert_builder (this stage is
# FROM litert_builder); re-declared here so the stage's inputs are explicit.
ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr

# Always create the deploy tree (zero-cost) so downstream COPY --from= lines
# succeed whether or not LiteRT-LM was actually built below.
RUN mkdir -p ${LITERT_LM_DEPLOY_DIR}/lib ${LITERT_LM_DEPLOY_DIR}/bin ${LITERT_LM_DEPLOY_DIR}/include

# Same toolchain registration as litert_builder above (re-generated — fresh
# stage layer, build-litert-lm-incremental.sh needs it too).
RUN { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo 'package(default_visibility = ["//visibility:public"])'; \
      echo ''; \
      echo 'toolchain('; \
      echo '    name = "cc_toolchain_aarch64",'; \
      echo '    exec_compatible_with = ['; \
      echo '        "@platforms//cpu:x86_64",'; \
      echo '        "@platforms//os:linux",'; \
      echo '    ],'; \
      echo '    target_compatible_with = ['; \
      echo '        "@platforms//cpu:aarch64",'; \
      echo '        "@platforms//os:linux",'; \
      echo '    ],'; \
      echo '    toolchain = "@local_config_embedded_arm//:cc-compiler-aarch64",'; \
      echo '    toolchain_type = "@bazel_tools//tools/cpp:toolchain_type",'; \
      echo ')'; \
    } > /tmp/litert-cross-compile-toolchains-BUILD

COPY qai-forge/scripts/deps/build-litert-lm-incremental.sh /tmp/
COPY qai-forge/scripts/deps/build-rust-wrappers.sh /tmp/
RUN chmod +x /tmp/build-litert-lm-incremental.sh /tmp/build-rust-wrappers.sh && \
    if [ "$BUILD_LITERT_LM" = "ON" ]; then \
        /tmp/build-litert-lm-incremental.sh; \
    else \
        echo "=== Skipping LiteRT-LM build (BUILD_LITERT_LM=OFF) ==="; \
    fi && \
    rm /tmp/build-litert-lm-incremental.sh /tmp/build-rust-wrappers.sh /tmp/litert-cross-compile-toolchains-BUILD

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM debian:trixie-slim AS builder

# Re-declare to bring the global ARG into this stage's scope.
ARG QAIRT_SDK_VERSION

# Cross-compile setup: this stage runs natively on $BUILDPLATFORM (x86_64)
# and cross-compiles fastrpc/Drogon/qai-forge/llama.cpp/QAIServe to aarch64
# instead of running under QEMU — find_package(gRPC REQUIRED)'s pkg-config
# resolution through re2/abseil was measured at ~28min under QEMU vs
# milliseconds natively. dpkg --add-architecture arm64 + :arm64 -dev packages
# below give aarch64 headers/libs for the cross compiler to link against;
# codegen tools that must run at build time stay native amd64.
RUN dpkg --add-architecture arm64

# Drogon + fastrpc build deps + binutils for strip. libatomic1:arm64 is a
# runtime dep of hexagon-clang (run under qemu-user during
# build-llamacpp-hexagon.sh) — without it hexagon-clang fails to start.
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
        crossbuild-essential-arm64 \
        libssl-dev:arm64 \
        libjsoncpp-dev:arm64 \
        uuid-dev:arm64 \
        zlib1g-dev:arm64 \
        libatomic1:arm64 \
        autoconf \
        automake \
        libtool \
        m4 \
        libbsd-dev:arm64 \
        libmd-dev:arm64 \
        libyaml-dev:arm64 \
        binutils \
        binutils-aarch64-linux-gnu \
        protobuf-compiler \
        libprotobuf-dev:arm64 \
        libgrpc++-dev:arm64 \
        protobuf-compiler-grpc \
        libc-ares-dev:arm64 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# pkg-config's default search path is the host's own (amd64) multiarch dir;
# point it at the arm64 one instead so it resolves the :arm64 -dev packages'
# .pc files above (re2.pc, libcares.pc, etc) — runs natively here, not under
# QEMU, so the abseil dependency graph inside re2.pc resolves in milliseconds
# instead of minutes.
ENV PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
ENV PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig

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
# Reuse the QAIRT SDK already downloaded/extracted by qairt_fetcher instead
# of having fetch-qairt-sdk.sh fetch its own copy — see the qairt_fetcher
# stage comment near the top of this file.
COPY --from=qairt_fetcher /opt/qairt-sdk ${QAIRT_DOWNLOAD_DIR}
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

# ── Build llama.cpp (provides llamacpp-inference-worker + libllama.so + libggml*.so) ──
# Uses modular script for maintainability and reusability. Skipped entirely
# when BUILD_LLAMACPP=OFF — no Hexagon SDK cross-toolchain setup, no
# llama.cpp clone/compile.
ENV LLAMACPP_INSTALL_DIR=/build/llamacpp-install
ARG BUILD_LLAMACPP
COPY qai-forge/scripts/deps/build-llamacpp-hexagon.sh /tmp/
RUN chmod +x /tmp/build-llamacpp-hexagon.sh && \
    if [ "$BUILD_LLAMACPP" = "ON" ]; then \
        /tmp/build-llamacpp-hexagon.sh; \
    else \
        echo "=== Skipping llama.cpp build (BUILD_LLAMACPP=OFF) ==="; \
    fi && \
    rm /tmp/build-llamacpp-hexagon.sh

# ── Pre-fetch nlohmann/json / stb / minja (NOT via CMake FetchContent) ───────
# qai-forge/CMakeLists.txt requires NLOHMANN_JSON_SOURCE_DIR and
# STB_SOURCE_DIR unconditionally, and MINJA_SOURCE_DIR whenever
# QAI_FORGE_BUILD_LITERT_LM=ON — all must be staged on disk *before* the
# `cmake -S` configure step below so that configuration never performs
# git/network activity. Pins match the commits previously hardcoded in
# qai-forge/CMakeLists.txt's FetchContent calls.
ENV NLOHMANN_JSON_SRC_DIR=/opt/third_party/nlohmann_json
ENV NLOHMANN_JSON_GIT_REF=9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
RUN git clone https://github.com/nlohmann/json.git ${NLOHMANN_JSON_SRC_DIR} && \
    cd ${NLOHMANN_JSON_SRC_DIR} && git checkout ${NLOHMANN_JSON_GIT_REF}

ENV STB_SRC_DIR=/opt/third_party/stb
ENV STB_GIT_REF=2c980bb59875b0d32144a71867fbdebb2f77cd20
RUN git clone https://github.com/nothings/stb.git ${STB_SRC_DIR} && \
    cd ${STB_SRC_DIR} && git checkout ${STB_GIT_REF}

ENV MINJA_SRC_DIR=/opt/third_party/minja
ENV MINJA_GIT_REF=021c2293c187789ef13d56c6cfd89c9b134fd80f
ARG BUILD_LITERT_LM
RUN if [ "$BUILD_LITERT_LM" = "ON" ]; then \
        git clone https://github.com/google/minja.git ${MINJA_SRC_DIR} && \
        cd ${MINJA_SRC_DIR} && git checkout ${MINJA_GIT_REF}; \
    fi

# ── Build qai-forge (shared SDK: Layers 2-4 + worker binaries) ───────────────
WORKDIR /build
COPY qai-forge /build/qai-forge

ARG BUILD_LITERT_LM
ARG BUILD_GENIE
ARG BUILD_QNN
ARG BUILD_SNPE
ARG BUILD_LITERT

# Pins to the aarch64 cross compiler (crossbuild-essential-arm64); falls
# back to the system compiler on a native arm64 build host, where
# aarch64-linux-gnu-gcc/g++ don't exist (same BUILD_ARCH gate as deps/*.sh).
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" = "arm64" ]; then \
        CROSS_CMAKE_ARGS=""; \
    else \
        CROSS_CMAKE_ARGS="-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++"; \
    fi && \
    cmake -S /build/qai-forge -B /build/qai-forge/build \
        ${CROSS_CMAKE_ARGS} \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENIE_INCLUDE_PATH=/usr/include \
        -DGENIE_LIB_PATH=/usr/lib \
        -DQAI_FORGE_BUILD_GENIE=${BUILD_GENIE} \
        -DQNN_INCLUDE_PATH=/usr/include \
        -DSNPE_INCLUDE_PATH=/usr/include \
        -DNLOHMANN_JSON_SOURCE_DIR=${NLOHMANN_JSON_SRC_DIR} \
        -DSTB_SOURCE_DIR=${STB_SRC_DIR} \
        -DMINJA_SOURCE_DIR=${MINJA_SRC_DIR} \
        -DQAI_FORGE_BUILD_QNN=${BUILD_QNN} \
        -DQAI_FORGE_BUILD_SNPE=${BUILD_SNPE} \
        -DQAI_FORGE_BUILD_LITERT=${BUILD_LITERT} \
        -DLITERT_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_LITERT_LM=${BUILD_LITERT_LM} \
        -DLITERT_LM_INCLUDE_PATH=/usr/include \
        -DQAI_FORGE_BUILD_LLAMACPP=${BUILD_LLAMACPP} \
        -DLLAMACPP_INCLUDE_PATH=${LLAMACPP_INSTALL_DIR}/include \
        -DLLAMACPP_LIB_PATH=${LLAMACPP_INSTALL_DIR}/lib \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/qai-forge/build -j$(nproc) && \
    cmake --install /build/qai-forge/build --prefix ${DEPLOY_DIR}/usr

# When BUILD_LITERT_LM=OFF, neither litert_lm_builder (Stage 0b,
# skipped above) nor qai-forge's CMakeLists (litert-lm-inference-worker,
# gated on the same flag) produce any LiteRT-LM binaries — stage stubs for
# all three so the unconditional `COPY --from=builder ...` lines below still
# succeed, matching the stub convention build-litert-lm-incremental.sh
# already uses for litert-lm/litert-lm-advanced when
# BUILD_LITERT_LM_CLI_TOOLS=false.
RUN mkdir -p ${DEPLOY_DIR}/usr/bin && \
    for tool in litert-lm-inference-worker litert-lm litert-lm-advanced; do \
        if [ ! -f ${DEPLOY_DIR}/usr/bin/${tool} ]; then \
            printf '#!/bin/sh\necho "%s was not built into this image (BUILD_LITERT_LM=OFF)."\nexit 1\n' "${tool}" \
                > ${DEPLOY_DIR}/usr/bin/${tool} && \
            chmod +x ${DEPLOY_DIR}/usr/bin/${tool}; \
        fi; \
    done

# When BUILD_LLAMACPP=OFF, qai-forge's CMakeLists (gated on the same flag) doesn't
# build it — stage a stub so the unconditional `COPY --from=builder
# .../llamacpp-inference-worker` below still succeeds.
RUN mkdir -p ${DEPLOY_DIR}/usr/bin && \
    if [ ! -f ${DEPLOY_DIR}/usr/bin/llamacpp-inference-worker ]; then \
        printf '#!/bin/sh\necho "llamacpp-inference-worker was not built into this image (BUILD_LLAMACPP=OFF)."\nexit 1\n' \
            > ${DEPLOY_DIR}/usr/bin/llamacpp-inference-worker && \
        chmod +x ${DEPLOY_DIR}/usr/bin/llamacpp-inference-worker; \
    fi

# ── Install QAIServe-specific build deps (CURL + libzip) ─────────────────────
# Placed here (just before COPY QAIServe) so that changes to this step do not
# invalidate the expensive LiteRT / LiteRT-LM / qai-forge build cache above.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libcurl4-openssl-dev:arm64 \
        libzip-dev:arm64 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* \
    && touch /usr/bin/zipcmp /usr/bin/zipmerge /usr/bin/ziptool \
    && chmod +x /usr/bin/zipcmp /usr/bin/zipmerge /usr/bin/ziptool

# ── Build QAIServe server (Layer 1: Drogon HTTP server) ──────────────────────
COPY QAIServe /build/QAIServe

# Same cross-compiler pinning as qai-forge above. CMAKE_SYSTEM_NAME/
# _PROCESSOR also set here (unlike above) because Drogon's
# FindFilesystem.cmake try_run()s a compiled test program, which needs
# CMake to know this is a cross-compile rather than attempting to execute
# the aarch64 binary directly on this amd64 host.
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" = "arm64" ]; then \
        CROSS_CMAKE_ARGS=""; \
    else \
        CROSS_CMAKE_ARGS="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++"; \
    fi && \
    cmake -S /build/QAIServe -B /build/QAIServe/build \
        ${CROSS_CMAKE_ARGS} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/QAIServe/build -j$(nproc) && \
    cmake --install /build/QAIServe/build

# Strips our own binaries + .so files (not rfsa/adsp Hexagon DSP blobs —
# ARM64 strip would corrupt them). Uses aarch64 cross-strip since this
# stage's native `strip` can't operate on cross-compiled ELF files.
RUN find ${DEPLOY_DIR}/usr/bin -type f \
        -exec aarch64-linux-gnu-strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec aarch64-linux-gnu-strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib/postproc_plugins -maxdepth 1 -type f -name "*.so" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib/qai-forge/plugins -maxdepth 1 -type f -name "*.so" \
        -exec strip --strip-unneeded {} \; 2>/dev/null || true

# Stage 2: Stager — installs runtime apt packages solely to obtain their
# .so files, then cherry-picks only the required libraries into /export.
# $BUILDPLATFORM: no target-arch code ever executes (.so files just copied).
FROM --platform=$BUILDPLATFORM debian:trixie-slim AS stager

# Same value as the builder stage's DEPLOY_DIR — re-declared here since ENV
# does not cross stage boundaries and this stage is FROM debian:trixie-slim,
# not FROM builder.
ENV DEPLOY_DIR=/build/deploy

RUN dpkg --add-architecture arm64

# Trixie's gRPC/protobuf runtime packages carry the 64-bit time_t suffix
# (libgrpc++1.51t64 etc.) — the plain bookworm names don't exist on trixie
# and silently resolve to nothing. abseil/re2/c-ares/upb pulled in
# transitively, cherry-picked below by SONAME.
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libyaml-0-2:arm64 \
        libbsd0:arm64 \
        libmd0:arm64 \
        libjsoncpp26:arm64 \
        libssl3:arm64 \
        zlib1g:arm64 \
        libuuid1:arm64 \
        libatomic1:arm64 \
        libcurl4:arm64 \
        libzip5:arm64 \
        libnghttp2-14:arm64 \
        libnghttp3-9:arm64 \
        librtmp1:arm64 \
        libidn2-0:arm64 \
        libssh2-1:arm64 \
        libpsl5:arm64 \
        libgssapi-krb5-2:arm64 \
        libldap2:arm64 \
        libbrotli1:arm64 \
        libzstd1:arm64 \
        liblzma5:arm64 \
        libbz2-1.0:arm64 \
        libsasl2-2:arm64 \
        libunistring5:arm64 \
        libgnutls30:arm64 \
        libgrpc++1.51t64:arm64 \
        libgrpc29t64:arm64 \
        libprotobuf32t64:arm64 \
        libgomp1:arm64 \
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
COPY --from=builder ${DEPLOY_DIR}/usr/bin/llamacpp-inference-worker /export/usr/bin/
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

# Stage 3: Runtime (distroless/cc-debian13) — same glibc 2.41 + GCC 14
# libstdc++ as the trixie-slim builder/stager stages (required by the
# prebuilt GenieX SDK's GLIBC_2.39/GLIBCXX_3.4.32 deps).
FROM --platform=linux/arm64 gcr.io/distroless/cc-debian13 AS qaiserve-runtime

# Brings in all server/worker binaries, libqai_forge.so + runtime .so deps
# (Drogon, GenIE, QNN/SNPE/LiteRT, llama.cpp), Hexagon DSP blobs, and
# fastrpc config from the stager's /export tree.
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
ENV LLAMACPP_WORKER_BINARY=/usr/bin/llamacpp-inference-worker
# Directory PluginLoader scans at startup for qaiforge_plugin_*.so files
# (RuntimePluginRegistry). Matches QAI_FORGE_PLUGIN_INSTALL_SUBDIR's install
# location relative to CMAKE_INSTALL_PREFIX in qai-forge/CMakeLists.txt —
# already the compiled-in default, set explicitly here for clarity/override.
ENV QAI_FORGE_PLUGIN_DIR=/usr/lib/qai-forge/plugins
ENV LOG_LEVEL=ERROR
ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
# Active-worker silence threshold — override in docker-compose.yaml as needed
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
