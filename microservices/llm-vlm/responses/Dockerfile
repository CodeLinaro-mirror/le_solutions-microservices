# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# Build arguments for modular backend configuration
# ─────────────────────────────────────────────────────────────────────────────
ARG BUILD_LITERT_LM=true
ARG BUILD_LLAMACPP=false
ARG QAIRT_SDK_VERSION=2.48.0.260626
ARG BUILD_GENIE=true
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
# Cross-compiles LiteRT-LM for AArch64 from a native BUILDPLATFORM container
# when BUILD_LITERT_LM=true — see QAIServe/Dockerfile's litert_lm_builder
# stage and build-litert-lm-standalone.sh for the same rationale (avoids
# QEMU-user emulation entirely; --platform=$BUILDPLATFORM, not
# linux/arm64, pins this stage to the build host's own architecture
# regardless of the final image's target platform).
# Uses modular scripts for maintainability and reusability.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS litert_lm_builder

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

# Registers LiteRT's legacy `local_config_embedded_arm` cc_toolchain as a
# Bazel 7 platform-aware toolchain — required for --platforms=...:linux_aarch64
# toolchain resolution to succeed when cross-compiling (consumed by
# build-litert-lm-standalone.sh's BUILD_ARCH branch; see
# QAIServe/Dockerfile's litert_builder stage for the fuller rationale).
# Generated inline via RUN instead of COPY so no extra file needs to be
# added to the repo. Only meaningful on the cross-compile path — harmless
# but unused on a native arm64 build host.
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

COPY qai-forge/scripts/deps/build-litert-lm-standalone.sh /tmp/
COPY qai-forge/scripts/deps/build-rust-wrappers.sh /tmp/
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        chmod +x /tmp/build-litert-lm-standalone.sh && \
        /tmp/build-litert-lm-standalone.sh && \
        rm /tmp/build-litert-lm-standalone.sh && \
        chmod +x /tmp/build-rust-wrappers.sh && \
        /tmp/build-rust-wrappers.sh && \
        rm /tmp/build-rust-wrappers.sh; \
        rm -f /tmp/litert-cross-compile-toolchains-BUILD; \
    fi

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1: llama.cpp Builder (conditional via shell if)
#
# Cross-compiles llama.cpp with Hexagon NPU backend for AArch64 when
# BUILD_LLAMACPP=true — see QAIServe/Dockerfile's builder stage and
# build-llamacpp-hexagon.sh for the same BUILD_ARCH-gated cross-compile
# pattern (no QEMU: qaic and hexagon-clang both run natively on this stage's
# own x86_64/aarch64 architecture as appropriate, never under emulation).
# When BUILD_LLAMACPP=false, only creates minimal directory structure (zero build cost).
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS llamacpp_builder

# Re-declare ARG after FROM
ARG BUILD_LLAMACPP=false

ENV LLAMACPP_DEPLOY_DIR=/mnt/work/deploy/usr
RUN mkdir -p ${LLAMACPP_DEPLOY_DIR}/bin ${LLAMACPP_DEPLOY_DIR}/lib ${LLAMACPP_DEPLOY_DIR}/lib/rfsa/adsp ${LLAMACPP_DEPLOY_DIR}/include

# Conditional build using modular script
#
# dpkg --add-architecture arm64 + :arm64 suffixed -dev packages: this stage
# cross-compiles to aarch64 (crossbuild-essential-arm64's aarch64-linux-gnu-gcc)
# rather than building under QEMU, so fastrpc's link step (inside
# build-llamacpp-hexagon.sh) needs aarch64 versions of libbsd/libmd/libyaml —
# same pattern as QAIServe/Dockerfile's builder stage. libatomic1:arm64 and
# zlib1g:arm64 are runtime deps of the Hexagon SDK's hexagon-clang (an
# aarch64 binary run directly — no QEMU) during the HTP DSP skel
# compilation; without them hexagon-clang fails to even start ("error while
# loading shared libraries: libatomic.so.1" / "libz.so.1"), since
# crossbuild-essential-arm64 only provides libatomic's static .a for
# linking, not its runtime .so, and doesn't provide zlib at all.
# qemu-user-static is no longer needed: qaic (an x86_64 binary) now runs
# natively on this x86_64 build host instead of under emulation on a native
# arm64 host.
#
# Ubuntu (unlike Debian) does not mirror arm64 packages on archive.ubuntu.com
# — they live on ports.ubuntu.com under a separate suite layout — so
# dpkg --add-architecture arm64 alone leaves apt-get unable to find any
# :arm64 package ("Unable to locate package"). Restrict the existing sources
# to amd64 and add a ports.ubuntu.com arm64 source alongside it.
RUN if [ "$BUILD_LLAMACPP" = "true" ]; then \
        echo "=== Building llama.cpp (BUILD_LLAMACPP=true) ===" && \
        dpkg --add-architecture arm64 && \
        UBUNTU_CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")" && \
        if [ -s /etc/apt/sources.list.d/ubuntu.sources ]; then \
            sed -i '/^Types:/i Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources; \
        fi && \
        if [ -s /etc/apt/sources.list ]; then \
            sed -i 's/^deb /deb [arch=amd64] /' /etc/apt/sources.list; \
        fi && \
        { echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME} main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-updates main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-security main restricted universe multiverse"; \
        } > /etc/apt/sources.list.d/arm64.list && \
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
            crossbuild-essential-arm64 \
            autoconf \
            automake \
            libtool \
            m4 \
            libbsd-dev:arm64 \
            libmd-dev:arm64 \
            libyaml-dev:arm64 \
            libatomic1:arm64 \
            zlib1g:arm64 \
            binutils \
            python3 \
            python3-pip \
            python-is-python3 \
        && apt-get clean \
        && rm -rf /var/lib/apt/lists/* \
        && pip3 install 'cmake<4'; \
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
#
# Cross-compiles fastrpc/Drogon/qai-forge/responses-server to aarch64 from a
# native BUILDPLATFORM (x86_64) container instead of running them all under
# QEMU-user emulation — same rationale as QAIServe/Dockerfile's builder
# stage.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS builder

ARG QAIRT_SDK_VERSION

#
# NOTE: ubuntu:22.04's apt-packaged cmake is 3.22.1, which is below the
# 3.23 minimum required by qai-forge/CMakeLists.txt (needed for jinja2cpp's
# own cmake_minimum_required). Upgrade via pip3 after the apt install,
# mirroring the same workaround already used in the llamacpp_builder stage
# above. Pinned to <4 — CMake 4.0 removed support entirely for projects
# whose cmake_minimum_required() is below 3.5 (previously just a warning);
# qai-forge's FetchContent-pulled jinja2cpp bundles a vendored rapidjson
# with exactly that, so an unpinned `pip3 install cmake --upgrade` grabs
# whatever is newest on PyPI and eventually breaks that dependency's
# configure step outright ("Compatibility with CMake < 3.5 has been
# removed"). 3.31.x (the latest 3.x line) still satisfies the >=3.23
# requirement above.
#
# dpkg --add-architecture arm64 + :arm64 suffixed -dev packages give us
# aarch64 headers/libs under the Debian/Ubuntu multiarch path
# (/usr/lib/aarch64-linux-gnu), which the aarch64-linux-gnu-gcc/g++ cross
# compiler (from crossbuild-essential-arm64) links against — same pattern as
# QAIServe/Dockerfile's builder stage.
#
# Ubuntu (unlike Debian) does not mirror arm64 packages on archive.ubuntu.com
# — they live on ports.ubuntu.com under a separate suite layout — so
# dpkg --add-architecture arm64 alone leaves apt-get unable to find any
# :arm64 package. Restrict the existing sources to amd64 and add a
# ports.ubuntu.com arm64 source alongside it.
#
# Same BUILD_ARCH gate as the cmake/strip steps below: on a native arm64
# build host, ubuntu:22.04's default sources.list is already the
# ports.ubuntu.com arm64 mirror, so this rewrite is unneeded — and actively
# wrong, since it would mistag those already-arm64 sources as arch=amd64 and
# apt-get would then 404 on nonexistent binary-amd64 index files.
# Install build dependencies
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" != "arm64" ]; then \
        dpkg --add-architecture arm64 && \
        UBUNTU_CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")" && \
        if [ -s /etc/apt/sources.list.d/ubuntu.sources ]; then \
            sed -i '/^Types:/i Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources; \
        fi && \
        if [ -s /etc/apt/sources.list ]; then \
            sed -i 's/^deb /deb [arch=amd64] /' /etc/apt/sources.list; \
        fi && \
        { echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME} main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-updates main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-security main restricted universe multiverse"; \
        } > /etc/apt/sources.list.d/arm64.list; \
    fi && \
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
        crossbuild-essential-arm64 \
        libssl-dev:arm64 \
        libjsoncpp-dev:arm64 \
        uuid-dev:arm64 \
        zlib1g-dev:arm64 \
        nlohmann-json3-dev:arm64 \
        autoconf \
        automake \
        libtool \
        m4 \
        libbsd-dev:arm64 \
        libmd-dev:arm64 \
        libyaml-dev:arm64 \
        binutils \
        binutils-aarch64-linux-gnu \
        python3 \
        python3-pip \
        python-is-python3 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install 'cmake<4'

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
RUN if [ "$BUILD_LITERT_LM" = "true" ]; then \
        git clone https://github.com/google/minja.git ${MINJA_SRC_DIR} && \
        cd ${MINJA_SRC_DIR} && git checkout ${MINJA_GIT_REF}; \
    fi

# Build qai-forge
WORKDIR /build
COPY qai-forge /build/qai-forge

ARG BUILD_LLAMACPP
ARG BUILD_GENIE

# CMAKE_C_COMPILER/CMAKE_CXX_COMPILER pin this build to the aarch64 cross
# compiler (crossbuild-essential-arm64, installed above) — same
# BUILD_ARCH-gated pattern as QAIServe/Dockerfile's qai-forge cmake call: a
# native arm64 build host (where this stage's own BUILD_ARCH is arm64) has
# no aarch64-linux-gnu-gcc/g++ (only plain gcc/g++), so it falls back to the
# system default compiler there.
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
        -DSTB_SOURCE_DIR=${STB_SRC_DIR} \
        -DMINJA_SOURCE_DIR=${MINJA_SRC_DIR} \
        -DQAI_FORGE_BUILD_GENIE=${BUILD_GENIE} \
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

# Same BUILD_ARCH-gated cross-compiler pinning as the qai-forge cmake call
# above.
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" = "arm64" ]; then \
        CROSS_CMAKE_ARGS=""; \
    else \
        CROSS_CMAKE_ARGS="-DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++"; \
    fi && \
    cmake -S /build/responses -B /build/responses/build \
        ${CROSS_CMAKE_ARGS} \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=${DEPLOY_DIR}/usr \
        -DCMAKE_PREFIX_PATH=${DEPLOY_DIR}/usr && \
    cmake --build /build/responses/build -j$(nproc) && \
    cmake --install /build/responses/build

# Verify LiteRT-LM dependencies
#
# Uses readelf -d (static ELF inspection), not ldd — ldd works by executing
# the target file with LD_TRACE_LOADED_OBJECTS=1, which fails outright for a
# cross-compiled aarch64 .so on this x86_64 build host ("Exec format error")
# since this Dockerfile cross-compiles without QEMU. readelf reads the
# NEEDED entries from the ELF dynamic section directly, no execution
# required, and works identically whether this stage is cross-compiling or
# building natively on arm64.
RUN echo "Verifying LiteRT-LM library dependencies:" \
    && if [ -f "${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so" ]; then \
           readelf -d ${DEPLOY_DIR}/usr/lib/liblitert_lm_c_api.so || { \
               echo "ERROR: liblitert_lm_c_api.so has unresolved dependencies"; \
               exit 1; \
           }; \
       fi \
    && echo "✓ LiteRT-LM dependencies verified"

# Strip binaries
#
# Uses the aarch64 cross-strip when cross-compiling — this stage's native
# `strip` is an amd64 binary and can't operate on the aarch64 ELF files
# produced by cross-compilation (same pattern as QAIServe/Dockerfile).
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" = "arm64" ]; then \
        STRIP_BIN=strip; \
    else \
        STRIP_BIN=aarch64-linux-gnu-strip; \
    fi && \
    find ${DEPLOY_DIR}/usr/bin -type f \
        -exec "${STRIP_BIN}" --strip-unneeded {} \; 2>/dev/null || true && \
    find ${DEPLOY_DIR}/usr/lib -maxdepth 1 -type f -name "*.so*" \
        -exec "${STRIP_BIN}" --strip-unneeded {} \; 2>/dev/null || true

# ─────────────────────────────────────────────────────────────────────────────
# Stage 3: Stager
#
# --platform=$BUILDPLATFORM + :arm64 multiarch packages: apt's own dependency
# resolution is the only thing that executes here — we only cherry-pick .so
# files by copying them, never load/exec them — so running this natively on
# the build host avoids QEMU overhead for what is otherwise a pure
# download+extract step. Same pattern as QAIServe/Dockerfile's stager stage.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=$BUILDPLATFORM ubuntu:22.04 AS stager

ENV DEPLOY_DIR=/build/deploy

# Same BUILD_ARCH gate as the builder stage: on a native arm64 build host,
# ubuntu:22.04's default sources.list is already the ports.ubuntu.com arm64
# mirror, so this rewrite is unneeded — and actively wrong, since it would
# mistag those already-arm64 sources as arch=amd64 and apt-get would then
# 404 on nonexistent binary-amd64 index files.
RUN BUILD_ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)" && \
    if [ "${BUILD_ARCH}" != "arm64" ]; then \
        dpkg --add-architecture arm64 && \
        UBUNTU_CODENAME="$(. /etc/os-release && echo "${VERSION_CODENAME}")" && \
        if [ -s /etc/apt/sources.list.d/ubuntu.sources ]; then \
            sed -i '/^Types:/i Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources; \
        fi && \
        if [ -s /etc/apt/sources.list ]; then \
            sed -i 's/^deb /deb [arch=amd64] /' /etc/apt/sources.list; \
        fi && \
        { echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME} main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-updates main restricted universe multiverse"; \
          echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports ${UBUNTU_CODENAME}-security main restricted universe multiverse"; \
        } > /etc/apt/sources.list.d/arm64.list; \
    fi

# Install runtime deps
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libyaml-0-2:arm64 \
        libbsd0:arm64 \
        libmd0:arm64 \
        libjsoncpp25:arm64 \
        libssl3:arm64 \
        zlib1g:arm64 \
        libuuid1:arm64 \
        libatomic1:arm64 \
        libgomp1:arm64 \
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
ENV GENAI_WORKER_ACTIVE_TIMEOUT=600

EXPOSE 9002

WORKDIR /mnt/work/models

ENTRYPOINT ["/usr/bin/responses-server"]

ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
