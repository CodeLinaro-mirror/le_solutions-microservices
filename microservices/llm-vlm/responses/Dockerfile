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
# Three-stage build for minimal image size:
#   Stage 1 (builder):  Compile everything; strip our own ELF artifacts.
#   Stage 2 (stager):   Cherry-pick only the required system .so files from
#                       a full Debian environment; assemble /export tree.
#   Stage 3 (runtime):  gcr.io/distroless/cc-debian12 — COPY /export in.
#                       No shell, no package manager, no apt cache.
#
#   Privilege drop: handled natively by the C++ server binary at startup.
# ─────────────────────────────────────────────────────────────────────────────

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

# ── Build qai-forge (shared SDK: Layers 2-4 + worker binaries) ───────────────
WORKDIR /build
COPY qai-forge /build/qai-forge

RUN cmake -S /build/qai-forge -B /build/qai-forge/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGENIE_INCLUDE_PATH=/usr/include \
        -DGENIE_LIB_PATH=/usr/lib \
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
      echo '# SPDX-License-Identifier:BSD-3-Clause-Clear'; \
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
#                 genai-vlm-inference-worker
#                 (qai-forge-daemon excluded — workers are spawned directly)
#   /usr/lib/   - libqai_forge.so, libdrogon.so, libtrantor.so,
#                 libGenie.so, QNN DSP/HTP/HTA aarch64 libs (GPU/LPAI/SNPE excluded),
#                 libcdsprpc.so*, libadsprpc.so*,
#                 cherry-picked system .so files
#   /usr/lib/rfsa/adsp/ - Hexagon DSP blobs (v68/v73/v75)
#   /etc/fastrpc/       - hexagon-dsp-binaries.yaml
COPY --from=stager /export /

# Set environment defaults
ENV RESPONSES_PORT=9002
ENV GENAI_MODELS_DIR=/mnt/work/models
ENV GENAI_WORKER_BINARY=/usr/bin/genai-inference-worker
ENV GENAI_VLM_WORKER_BINARY=/usr/bin/genai-vlm-inference-worker
ENV LOG_LEVEL=ERROR

EXPOSE 9002

WORKDIR /mnt/work/models

# The C++ server binary is the entrypoint.
# It calls drop_privileges_and_bind_devices() before starting Drogon.
# Child processes (genai-inference-worker) inherit the dropped privileges.
ENTRYPOINT ["/usr/bin/responses-server"]
