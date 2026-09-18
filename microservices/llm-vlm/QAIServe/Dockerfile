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
#   BUILD_LITERT_LM     Whether to build LiteRT-LM at all. When OFF
#                       (the default), Stage 0b (litert_lm_builder) skips
#                       build-litert-lm-incremental.sh entirely — no Bazel
#                       invocation, no LiteRT-LM clone — and qai-forge's
#                       litert-lm-inference-worker is not compiled. Pass
#                       --build-arg BUILD_LITERT_LM=ON to build and
#                       ship LiteRT-LM support again.
#
#   BUILD_LITERT_LM_CLI_TOOLS  Build the litert-lm / litert-lm-advanced
#                       on-device debugging/benchmarking CLI binaries
#                       (Stage 0b). Only relevant when
#                       BUILD_LITERT_LM=ON — QAIServe never calls
#                       these at runtime, it drives LiteRT-LM inference via
#                       liblitert_lm_c_api.so through litert-lm-inference-worker.
#                       Left off by default to keep the image small; pass
#                       --build-arg BUILD_LITERT_LM_CLI_TOOLS=true to build
#                       and ship them again.
#
#   BUILD_LLAMACPP      Whether to build llama.cpp with Hexagon NPU support.
#                       Defaults to ON (the current NPU backend). When OFF,
#                       the llama.cpp build step in Stage 1 (builder) skips
#                       build-llamacpp-hexagon.sh entirely — no Hexagon SDK
#                       cross-toolchain setup, no llama.cpp clone/compile —
#                       and qai-forge's llamacpp-inference-worker is not
#                       compiled. Pass --build-arg BUILD_LLAMACPP=OFF to skip
#                       it.
#
#   BAZEL_JOBS          Bazel action concurrency for Stage 0 / 0b. Both stages
#                       now cross-compile natively on the x86_64 build host
#                       (see below) instead of running an AArch64 Bazel under
#                       QEMU, so this can be $(nproc) — it no longer needs to
#                       be throttled to avoid the QEMU-user emulation deadlock
#                       that motivated the old default of 4.
#
# ─────────────────────────────────────────────────────────────────────────────
ARG QAIRT_SDK_VERSION=2.48.0.260626
ARG LITERT_VERSION=2.1.6
ARG LITERT_LM_VERSION=0.14.0
ARG BUILD_LITERT_LM=ON
ARG BUILD_LITERT_LM_CLI_TOOLS=false
ARG BUILD_LLAMACPP=ON
ARG BUILD_GENIE=true
ARG BAZEL_JOBS=4

# ─────────────────────────────────────────────────────────────────────────────
# Stage -1: qairt_fetcher
#
# Downloads and extracts the QAIRT SDK zip exactly once. litert_builder
# (build-litert.sh) and builder (fetch-qairt-sdk.sh) both need a copy of the
# same QAIRT_SDK_VERSION but run in unrelated stages with no shared
# filesystem — without this, each would independently download the same
# multi-hundred-MB zip from softwarecenter.qualcomm.com. They instead
# COPY --from=qairt_fetcher the already-extracted tree; both scripts detect
# it's already present at their expected path and skip the network fetch.
#
# --platform=$BUILDPLATFORM (not linux/arm64): this stage only runs wget/unzip
# — no target-architecture code ever executes, so pinning it to arm64 just
# means paying QEMU overhead for zero benefit (the SDK zip's contents are
# architecture-agnostic at this stage; individual .so files inside are picked
# up by other stages that already handle their own architecture). Building
# natively on $BUILDPLATFORM (x86_64) avoids that overhead entirely.
# ─────────────────────────────────────────────────────────────────────────────
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

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0: litert_builder
#
# Cross-compiles LiteRT for AArch64 from a native BUILDPLATFORM container (not
# --platform=linux/arm64 — see build-litert.sh for why). --platform=$BUILDPLATFORM
# is required here, not just omitting --platform: a bare `FROM` with no
# --platform inherits the overall build's --platform (arm64, from
# docker-compose.yaml/buildx), which would put us right back under QEMU.
# $BUILDPLATFORM pins this stage to the builder host's own architecture
# (x86_64) regardless of the target platform requested for the final image.
# Uses Bazelisk's x86_64 build to drive an aarch64-targeting Bazel build via
# LiteRT's elinux_aarch64 config, so no QEMU emulation is involved in the
# build itself. Uses modular script for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
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

# Registers LiteRT's legacy `local_config_embedded_arm` cc_toolchain as a
# Bazel 7 platform-aware toolchain — required for --platforms=...:linux_aarch64
# toolchain resolution to succeed when cross-compiling (see build-litert.sh's
# BUILD_ARCH branch for where this file is consumed). Generated inline via
# RUN instead of COPY so no extra file needs to be added to the repo.
RUN { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo '# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.'; \
      echo '#'; \
      echo '# Registers LiteRT'"'"'s legacy `local_config_embedded_arm` cc_toolchain (its'; \
      echo '# elinux_aarch64 config, built on Bazel'"'"'s hermetic ARM GNU-A download) as a'; \
      echo '# Bazel 7 platform-aware toolchain.'; \
      echo '#'; \
      echo '# Without this, --platforms=...:linux_aarch64 fails toolchain resolution'; \
      echo '# ("No matching toolchains found") because local_config_embedded_arm only'; \
      echo '# registers under the legacy --crosstool_top mechanism. And without'; \
      echo '# --platforms=...:linux_aarch64, LiteRT'"'"'s own config_setting()s — which use'; \
      echo '# constraint_values, not the legacy --cpu flag — misresolve to the host'; \
      echo '# (x86_64) arch and leak x86-only flags like -msse4.2 into the aarch64'; \
      echo '# compile. Both the legacy --cpu/--crosstool_top flags (to pick the compiler)'; \
      echo '# and this toolchain() registration + --platforms (to make config_setting()'; \
      echo '# resolve correctly) are required together; see build-litert.sh.'; \
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

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0b: litert_lm_builder
#
# Builds LiteRT-LM incrementally on top of the already-built LiteRT, but only
# when BUILD_LITERT_LM=ON. When OFF, this stage does not clone
# LiteRT-LM or invoke Bazel at all — it just stages an empty directory tree so
# the unconditional `COPY --from=litert_lm_builder` lines below the builder
# stage still succeed.
# Cross-compiles LiteRT-LM for AArch64, same approach as litert_builder above
# (inherits its BUILDPLATFORM/x86_64 environment since this stage is FROM
# litert_builder). See build-litert-lm-incremental.sh for the LiteRT-LM
# specific cross-compile flags (elinux_aarch64 config, Rust toolchain triple,
# -fpermissive) — its WORKSPACE/.bazelrc differ enough from LiteRT's that the
# same flag set doesn't carry over unchanged.
# Uses modular script for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
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

# Same platform-aware cc_toolchain registration as litert_builder above —
# re-generated here (not persisted from the previous stage) since this is a
# fresh FROM litert_builder layer and build-litert-lm-incremental.sh's own
# cross-compile branch also needs it at /tmp/litert-cross-compile-toolchains-BUILD.
RUN { \
      echo '# SPDX-License-Identifier: BSD-3-Clause-Clear'; \
      echo '# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.'; \
      echo '#'; \
      echo '# Registers LiteRT'"'"'s legacy `local_config_embedded_arm` cc_toolchain (its'; \
      echo '# elinux_aarch64 config, built on Bazel'"'"'s hermetic ARM GNU-A download) as a'; \
      echo '# Bazel 7 platform-aware toolchain.'; \
      echo '#'; \
      echo '# Without this, --platforms=...:linux_aarch64 fails toolchain resolution'; \
      echo '# ("No matching toolchains found") because local_config_embedded_arm only'; \
      echo '# registers under the legacy --crosstool_top mechanism. And without'; \
      echo '# --platforms=...:linux_aarch64, LiteRT'"'"'s own config_setting()s — which use'; \
      echo '# constraint_values, not the legacy --cpu flag — misresolve to the host'; \
      echo '# (x86_64) arch and leak x86-only flags like -msse4.2 into the aarch64'; \
      echo '# compile. Both the legacy --cpu/--crosstool_top flags (to pick the compiler)'; \
      echo '# and this toolchain() registration + --platforms (to make config_setting()'; \
      echo '# resolve correctly) are required together; see build-litert.sh.'; \
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

# ─────────────────────────────────────────────────────────────────────────────
# Cross-compile setup: this stage runs natively on the build host's own
# architecture ($BUILDPLATFORM, x86_64) and cross-compiles fastrpc/Drogon/
# qai-forge/llama.cpp/QAIServe to aarch64, instead of running them all under
# QEMU-user emulation. This was motivated by the QAIServe CMake configure
# step (find_package(gRPC REQUIRED)) recursing through re2's ~228-package
# abseil dependency graph via pkg-config — each pkg-config exec costs
# 1-2 minutes under QEMU (measured: ~86s for a single `pkg-config --libs
# re2`), adding up to ~28 minutes of configure time; the exact same
# resolution is millisecond-fast when pkg-config itself runs natively,
# regardless of which architecture's .pc files it's reading.
#
# dpkg --add-architecture arm64 + :arm64 suffixed -dev packages below give
# us aarch64 headers/libs under the Debian multiarch path
# (/usr/lib/aarch64-linux-gnu), which the aarch64-linux-gnu-gcc/g++ cross
# compiler (from crossbuild-essential-arm64) links against; codegen tools
# that must run at build time (protobuf-compiler, protobuf-compiler-grpc)
# stay native amd64.
# ─────────────────────────────────────────────────────────────────────────────
RUN dpkg --add-architecture arm64

# Install build dependencies:
#   - Drogon hard deps (no DB client libs)
#   - fastrpc build deps: autoconf/automake/libtool/libbsd-dev/libmd-dev/libyaml-dev
#   - binutils for strip
#   - libatomic1:arm64: runtime dep of the Hexagon SDK's hexagon-clang (an
#     aarch64 binary run under qemu-user during build-llamacpp-hexagon.sh's
#     HTP DSP skel compilation) — without it hexagon-clang fails to even
#     start ("error while loading shared libraries: libatomic.so.1"), since
#     crossbuild-essential-arm64 only provides the static .a for linking, not
#     this runtime .so.
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
        nlohmann-json3-dev:arm64 \
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

# ── Pre-fetch stb / minja (NOT via CMake FetchContent) ───────────────────────
# qai-forge/CMakeLists.txt requires STB_SOURCE_DIR unconditionally and
# MINJA_SOURCE_DIR whenever QAI_FORGE_BUILD_LITERT_LM=ON — both must be
# staged on disk *before* the `cmake -S` configure step below so that
# configuration never performs git/network activity. Pins match the
# commits previously hardcoded in qai-forge/CMakeLists.txt's FetchContent
# calls.
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

# CMAKE_C_COMPILER/CMAKE_CXX_COMPILER pin this build to the aarch64 cross
# compiler (crossbuild-essential-arm64, installed above) — no
# CMAKE_SYSTEM_NAME/CMAKE_FIND_ROOT_PATH toolchain-file ceremony needed;
# Debian's multiarch cross-gcc already resolves aarch64 headers/libs on its
# own (verified with a minimal find_package(gRPC REQUIRED) repro, see
# x86.html). All the -D*_INCLUDE_PATH/-D*_LIB_PATH flags below already point
# at aarch64 content (vendor QAIRT/GenieX .so + headers, or output staged by
# the cross-compiled litert_builder/litert_lm_builder Bazel stages /
# build-llamacpp.sh above), so only the compiler selection needs to change.
#
# BUILD_ARCH-gated: unlike the deps/*.sh scripts, this compiler selection was
# originally hardcoded unconditionally here, which breaks a native arm64
# build host (this stage runs on $BUILDPLATFORM, which equals arm64 on such a
# host) — aarch64-linux-gnu-gcc/g++ are cross-compiler binary names that
# don't exist on a native arm64 system (which only has plain gcc/g++),
# causing "Could NOT find CMAKE_C_COMPILER". Gated the same way as
# build-drogon.sh/build-llamacpp.sh so a native arm64 build host falls back
# to the system default compiler untouched.
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
        -DSTB_SOURCE_DIR=${STB_SRC_DIR} \
        -DMINJA_SOURCE_DIR=${MINJA_SRC_DIR} \
        -DQAI_FORGE_BUILD_QNN=ON \
        -DQAI_FORGE_BUILD_SNPE=ON \
        -DQAI_FORGE_BUILD_LITERT=ON \
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

# Cross-compiles to aarch64 via the same aarch64-linux-gnu-gcc/g++ pinning as
# qai-forge above — running find_package(gRPC REQUIRED)'s pkg-config
# resolution natively here avoids the multi-minute QEMU pkg-config overhead
# (see qai-forge cmake call above for the measured numbers).
#
# BUILD_ARCH-gated the same way: native arm64 hosts fall back to the system
# compiler. CMAKE_SYSTEM_NAME/CMAKE_SYSTEM_PROCESSOR are required alongside
# the compiler vars so CMake knows this is a cross-compile — otherwise
# Drogon's FindFilesystem.cmake (via find_package(Drogon) below) try_run()s
# a compiled aarch64 test program on this amd64 host, which fails without
# qemu-user-static + binfmt_misc set up.
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

# ── Strip our own compiled ELF artifacts to reduce size ──────────────────────
# Strips debug symbols from:
#   - All executables in bin/ (our server + worker binaries)
#   - Our own compiled .so files (Drogon, trantor, qai_forge, fastrpc)
#   - Built-in postprocess plugin .so files
# Deliberately skips ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ — those are Hexagon DSP
# blobs compiled for the DSP core, not ARM64; stripping them would corrupt them.
# Vendor QAIRT .so files are already stripped release binaries; strip is a no-op.
# Uses the aarch64 cross-strip (from crossbuild-essential-arm64/
# binutils-aarch64-linux-gnu, installed above) — this stage's native `strip`
# is an amd64 binary and can't operate on the aarch64 ELF files produced by
# cross-compilation.
RUN find ${DEPLOY_DIR}/usr/bin -type f \
        -exec aarch64-linux-gnu-strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec aarch64-linux-gnu-strip --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib/postproc_plugins -maxdepth 1 -type f -name "*.so" \
        -exec aarch64-linux-gnu-strip --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2: Stager
#
# Installs runtime apt packages solely to obtain their .so files, then
# cherry-picks only the required libraries into /export. Nothing else from
# this stage's OS lands in the final image.
#
# --platform=$BUILDPLATFORM + :arm64 multiarch packages (same pattern as the
# builder stage — see x86.html problem 3b): apt's own dependency resolution
# and package post-install scripts are the only things that execute here: no
# target-architecture compiled code ever runs (we only cherry-pick .so files
# by copying them, never load/exec them). Running this natively on the build
# host avoids QEMU overhead for what is otherwise a pure download+extract
# step, identical in spirit to why qairt_fetcher was converted. Verified all
# these packages resolve cleanly as :arm64 on Debian trixie (apt correctly
# maps to the "t64" 64-bit-time_t renamed packages, e.g. libssl3 ->
# libssl3t64:arm64, libcurl4 -> libcurl4t64:arm64); ca-certificates is
# "Architecture: all" and needs no :arm64 suffix.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM debian:trixie-slim AS stager

# Same value as the builder stage's DEPLOY_DIR — re-declared here since ENV
# does not cross stage boundaries and this stage is FROM debian:trixie-slim,
# not FROM builder.
ENV DEPLOY_DIR=/build/deploy

RUN dpkg --add-architecture arm64

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
#                 litert-lm-inference-worker, litert-lm, litert-lm-advanced,
#                 llamacpp-inference-worker
#   /usr/lib/   - libqai_forge.so, libdrogon.so, libtrantor.so,
#                 libGenie.so, QNN, SNPE,
#                 liblitert_runtime_c_api.so, liblitertengine.so,
#                 LiteRT dispatch + compiler plugin .so files,
#                 libllama.so, libggml*.so (llama.cpp, CPU/Hexagon backends),
#                 libcdsprpc.so*, libadsprpc.so*,
#                 cherry-picked system .so files
#   /usr/lib/rfsa/adsp/ - Hexagon DSP blobs (v68/v73/v75, incl. libggml-htp-v*.so)
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
ENV LLAMACPP_WORKER_BINARY=/usr/bin/llamacpp-inference-worker
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
