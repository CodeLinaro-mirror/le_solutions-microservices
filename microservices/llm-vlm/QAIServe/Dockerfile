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
# ─────────────────────────────────────────────────────────────────────────────
ARG QAIRT_SDK_VERSION=2.45.40.260406

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0: litert_builder
#
# Builds LiteRT v2.1.5 natively on AArch64 using Bazelisk.
# Outputs staged artifacts to /mnt/work/deploy/usr:
#   /mnt/work/deploy/usr/lib/     — liblitert_runtime_c_api.so, dispatch .so, plugin .so
#   /mnt/work/deploy/usr/bin/     — run_model, run_model_simple, benchmark_model
#   /mnt/work/deploy/usr/include/ — litert/ and tflite/ C API headers
#
# Runs natively on ARM64 — no cross-compilation required.
# Artifacts are copied into the ARM64 builder stage below.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 ubuntu:22.04 AS litert_builder

# Re-declare to bring the global ARG into this stage's scope.
ARG QAIRT_SDK_VERSION

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        lld \
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
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Use ARM64 Bazelisk for native AArch64 build
ENV BAZELISK_VERSION=v1.28.1
RUN wget -t 3 -T 120 --no-verbose \
        -O /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-arm64" \
    && chmod +x /usr/local/bin/bazel

ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
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

# Clone LiteRT v2.1.5 with retry logic.
RUN git config --global http.postBuffer 1048576000 \
    && git config --global http.lowSpeedLimit 0 \
    && git config --global http.lowSpeedTime 999999 \
    && git config --global core.compression 0 \
    && for i in {1..5}; do \
           git clone --branch v2.1.5 --depth 1 https://github.com/google-ai-edge/LiteRT.git "${LITERT_SRC_DIR}" && break || { \
               echo "Clone failed, retrying in 15 seconds..."; \
               rm -rf "${LITERT_SRC_DIR}"; \
               sleep 15; \
           }; \
       done \
    && cd "${LITERT_SRC_DIR}" \
    && for i in {1..5}; do \
           git submodule update --init --recursive --depth 1 && break || { \
               echo "Submodule update failed, retrying in 15 seconds..."; \
               sleep 15; \
           }; \
       done \
    && if [ -f configure ]; then yes n | ./configure; \
       elif [ -f configure.py ]; then yes n | python3 configure.py; \
       else echo "No configure script found; using env-based configuration"; fi

# ── Download QAIRT ${QAIRT_SDK_VERSION} for LiteRT QNN dispatch + compiler plugin ──
# LiteRT v2.1.5 defaults to QAIRT 2.44.0.260225 (third_party/qairt/workspace.bzl).
# Setting LITERT_QAIRT_SDK overrides the default via configurable_repo's
# local_path_env mechanism — Bazel uses the local directory instead of
# downloading from softwarecenter.qualcomm.com.
#
# configurable_repo does string concatenation:
#   ctx.path(LITERT_QAIRT_SDK + strip_prefix)
# where strip_prefix = "qairt/2.44.0.260225"
# So LITERT_QAIRT_SDK must end with "/" and the path
#   ${LITERT_QAIRT_SDK}qairt/2.44.0.260225  must exist.
# We create a compatibility symlink: qairt/2.44.0.260225 → qairt/${QAIRT_SDK_VERSION}
ENV QAIRT_LITERT_VERSION=${QAIRT_SDK_VERSION}
ENV QAIRT_LITERT_DIR=/tmp/qairt-litert
RUN mkdir -p ${QAIRT_LITERT_DIR} \
    && wget -c -t 5 -T 120 --no-verbose \
        -P ${QAIRT_LITERT_DIR} \
        "https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_LITERT_VERSION}/v${QAIRT_LITERT_VERSION}.zip" \
    && cd ${QAIRT_LITERT_DIR} \
    && unzip v${QAIRT_LITERT_VERSION}.zip \
    && rm -f v${QAIRT_LITERT_VERSION}.zip \
    && ln -sf ${QAIRT_LITERT_DIR}/qairt/${QAIRT_LITERT_VERSION} \
              ${QAIRT_LITERT_DIR}/qairt/2.44.0.260225 \
    && echo "QAIRT ${QAIRT_LITERT_VERSION} ready; symlink 2.44.0.260225 → ${QAIRT_LITERT_VERSION}"

# LITERT_QAIRT_SDK must end with "/" so that:
#   LITERT_QAIRT_SDK + "qairt/2.44.0.260225" = a valid directory path.
ENV LITERT_QAIRT_SDK=${QAIRT_LITERT_DIR}/

WORKDIR ${LITERT_SRC_DIR}

# Prepare Bazel cache directory.
RUN mkdir -p /bazel-cache

# Native AArch64 build — no cross-compilation needed.
# Note: --config=linux already sets --cxxopt=-std=c++17 for C++ files.
# Do NOT pass --copt=-std=c++17 — it would be applied to C files too and fail.
RUN bazel build --config=linux -c opt \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --copt=-DLITERT_ENABLE_NPU \
    --linkopt=-Wl,--whole-archive \
    --linkopt=-Wl,--no-whole-archive \
    //litert/c:litert_runtime_c_api_so \
    //litert/c:litert_tensor_buffer \
    //litert/runtime:tensor_buffer \
    //litert/vendors/qualcomm/dispatch:dispatch_api_so \
    //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so \
    //litert/tools:run_model \
    //litert/tools:run_model_simple \
    //litert/tools:benchmark_model

# Stage LiteRT shared libraries, plugins, binaries, and public headers.
RUN mkdir -p "${LITERT_DEPLOY_DIR}/lib" "${LITERT_DEPLOY_DIR}/bin" "${LITERT_DEPLOY_DIR}/include" \
    && bazel cquery \
        --config=linux -c opt \
        --repo_env=HERMETIC_PYTHON_VERSION=${HERMETIC_PYTHON_VERSION} \
        --output=files \
        'set(//litert/c:litert_runtime_c_api_so //litert/c:litert_tensor_buffer //litert/runtime:tensor_buffer //litert/vendors/qualcomm/dispatch:dispatch_api_so //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so //litert/tools:run_model //litert/tools:run_model_simple //litert/tools:benchmark_model)' \
        > /tmp/litert_bazel_outputs.txt \
    && echo "Configured LiteRT Bazel outputs:" \
    && cat /tmp/litert_bazel_outputs.txt \
    && while IFS= read -r output; do \
            if [ -e "$output" ] || [ -L "$output" ]; then \
                case "$output" in \
                    *.so|*.so.*|*.a) cp -avL "$output" "${LITERT_DEPLOY_DIR}/lib/" ;; \
                    */run_model|*/run_model_simple|*/benchmark_model) cp -avL "$output" "${LITERT_DEPLOY_DIR}/bin/" ;; \
                esac; \
            fi; \
       done < /tmp/litert_bazel_outputs.txt \
    && find bazel-bin bazel-out \( -type f -o -type l \) \
        | grep -E '(/run_model|/run_model_simple|/benchmark_model)$' \
        | while IFS= read -r output; do cp -avL "$output" "${LITERT_DEPLOY_DIR}/bin/" || true; done \
    && cp -a litert "${LITERT_DEPLOY_DIR}/include/" \
    && cp -a tflite "${LITERT_DEPLOY_DIR}/include/" \
    && echo "Generating build_config.h from build_config.h.in (NPU enabled, GPU disabled)..." \
    && mkdir -p "${LITERT_DEPLOY_DIR}/include/litert/build_common" \
    && sed \
        -e 's/#cmakedefine01 LITERT_BUILD_CONFIG_DISABLE_GPU/#define LITERT_BUILD_CONFIG_DISABLE_GPU 1/' \
        -e 's/#cmakedefine01 LITERT_BUILD_CONFIG_DISABLE_NPU/#define LITERT_BUILD_CONFIG_DISABLE_NPU 0/' \
        litert/build_common/build_config.h.in \
        > "${LITERT_DEPLOY_DIR}/include/litert/build_common/build_config.h" \
    && echo "Staged LiteRT libraries and tools:" \
    && ls -lah "${LITERT_DEPLOY_DIR}/lib" \
    && ls -lah "${LITERT_DEPLOY_DIR}/bin" \
    && test "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f | wc -l)" -gt 0 \
    && test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*litert*.so' -o -iname '*litert*.so.*' -o -iname '*LiteRt*.so' -o -iname '*LiteRt*.so.*' -o -iname '*litert*.a' -o -iname '*LiteRt*.a' \) | head -1)" \
    && test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*dispatch*.so' -o -iname '*dispatch*.so.*' -o -iname '*qnn*plugin*.so' -o -iname '*qnn*plugin*.so.*' -o -iname '*dispatch*.a' -o -iname '*qnn*plugin*.a' \) | head -1)" \
    && test -f "${LITERT_DEPLOY_DIR}/bin/run_model" \
    && test -f "${LITERT_DEPLOY_DIR}/bin/run_model_simple" \
    && test -f "${LITERT_DEPLOY_DIR}/bin/benchmark_model"

# ─────────────────────────────────────────────────────────────────────────────
# Stage 0b: litert_lm_builder
#
# Builds LiteRT-LM incrementally on top of the already-built LiteRT.
# Starts FROM litert_builder — Docker layer caching means LiteRT is NOT
# rebuilt; only LiteRT-LM is compiled in this stage.
#
# Key: --override_repository=litert=${LITERT_SRC_DIR} tells LiteRT-LM's Bazel
# to use our already-cloned LiteRT source instead of downloading from GitHub.
# Bazel's action cache also reuses compiled LiteRT artifacts from Stage 0.
# ─────────────────────────────────────────────────────────────────────────────
FROM litert_builder AS litert_lm_builder

ENV LITERT_LM_SRC_DIR=/mnt/work/src/litert-lm
ENV LITERT_LM_DEPLOY_DIR=/mnt/work/deploy/usr

# Clone LiteRT-LM at tag v0.13.0 for a stable, reproducible build.
RUN git lfs install \
    && for i in {1..5}; do \
        git clone --branch v0.13.0 --depth 1 \
            https://github.com/google-ai-edge/LiteRT-LM.git "${LITERT_LM_SRC_DIR}" && break || { \
            echo "Clone failed, retrying in 15 seconds..."; \
            rm -rf "${LITERT_LM_SRC_DIR}"; \
            sleep 15; \
        }; \
    done

WORKDIR ${LITERT_LM_SRC_DIR}

# Ensure LFS artifacts are fully downloaded (so we don't get linker errors on pointer files)
RUN git lfs pull

# Copy patch files — placed here (after LiteRT build) so that patch changes
# do not invalidate the expensive LiteRT build cache layers above.
# COPY patches /mnt/work/src/patches

# Add cc_shared_library target for the LiteRT-LM C API directly.
# cc_shared_library statically links all transitive deps and exports the
# symbols from targets listed in exports_filter, producing a self-contained
# .so that CMake consumers can link against without needing companion .so files.
RUN printf '\n# Shared library target for the LiteRT-LM C API.\n# Produces liblitert_lm_c_api.so that can be loaded at runtime.\n# Used by litert-lm-inference-worker in the QAIServe service.\ncc_shared_library(\n    name = "litert_lm_c_api",\n    exports_filter = ["//c:engine"],\n    deps = [":engine"],\n    visibility = ["//visibility:public"],\n)\n' >> c/BUILD \
    && echo "Added cc_shared_library target to c/BUILD" \
    && tail -20 c/BUILD

# Build LiteRT-LM using Bazel, overriding the @litert repository to use our
# already-built LiteRT source (avoids a second LiteRT download/compile).
# LITERT_QAIRT_SDK is inherited from litert_builder so QAIRT 2.45.40.260406
# is also reused for the LiteRT-LM QNN dispatch path.
#
# Key flags:
#   --config=linux_arm64          ARM64 CPU optimization (-mtune=generic -march=armv8-a -O3)
#   --define=litert_enable_qnn    Enable QNN/NPU backend in LiteRT
#   --define=litert_link_capi_so  All LiteRT-LM targets use litert_api_with_dynamic_runtime
#                                 (dynamically loads liblitert_runtime_c_api.so at runtime)
#                                 instead of statically embedding LiteRT — required to avoid
#                                 symbol conflicts when liblitert_lm_c_api.so is loaded
#   --define=resolve_symbols_in_exec=false
#                                 Required companion to litert_link_capi_so=true: allows .so
#                                 files to have symbols resolved from other .so files
#                                 (liblitert_runtime_c_api.so) rather than from the executable
#   --linkopt=-fuse-ld=lld        Use LLD linker (already installed in Stage 0)
#
# //c:litert_lm_c_api produces liblitert_lm_c_api.so (added by patch above).
# //runtime/engine:litert_lm_main and litert_lm_advanced_main are CLI tools
# useful for on-device debugging and benchmarking.
# Build liblitert_lm_c_api.so as a self-contained shared library using
# cc_shared_library (the correct Bazel rule for this purpose).
# cc_shared_library statically links all transitive deps and exports the
# symbols from targets listed in exports_filter, producing a .so that
# CMake consumers can link against without needing 200+ companion .so files.
RUN bazel build \
    --config=linux \
    --config=linux_arm64 \
    --compilation_mode=opt \
    --override_repository=litert=${LITERT_SRC_DIR} \
    --define=litert_enable_qnn=true \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --linkopt=-fuse-ld=lld \
    --http_timeout_scaling=10 \
    --experimental_repository_downloader_retries=10 \
    //c:litert_lm_c_api

# Build the CLI tools separately with litert_link_capi_so=true so they
# dynamically load liblitert_runtime_c_api.so at runtime (avoids symbol conflicts).
RUN bazel build \
    --config=linux \
    --config=linux_arm64 \
    --compilation_mode=opt \
    --override_repository=litert=${LITERT_SRC_DIR} \
    --define=litert_enable_qnn=true \
    --define=litert_link_capi_so=true \
    --define=resolve_symbols_in_exec=false \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1 \
    --linkopt=-fuse-ld=lld \
    //runtime/engine:litert_lm_main \
    //runtime/engine:litert_lm_advanced_main

# Stage LiteRT-LM shared library, C API headers, and CLI tool binaries.
RUN mkdir -p "${LITERT_LM_DEPLOY_DIR}/lib" \
             "${LITERT_LM_DEPLOY_DIR}/bin" \
             "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c" \
    && echo "Searching for litert_lm_c_api .so in bazel outputs:" \
    && find -L bazel-bin bazel-out \( -type f -o -type l \) \
        \( -name "liblitert_lm_c_api.so" -o -name "liblitert_lm_c_api.so.*" \
           -o -name "litert_lm_c_api.so" -o -name "litert_lm_c_api.so.*" \
           -o -name "libGemmaModelConstraintProvider.so" \) \
        2>/dev/null | tee /tmp/litert_lm_so_files.txt \
    && echo "Copying found .so files:" \
    && while IFS= read -r f; do \
            base=$(basename "$f"); \
            if echo "$base" | grep -q "^lib"; then \
                cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/" || true; \
            else \
                cp -avL "$f" "${LITERT_LM_DEPLOY_DIR}/lib/lib${base}" || true; \
            fi; \
       done < /tmp/litert_lm_so_files.txt \
    && cp c/engine.h "${LITERT_LM_DEPLOY_DIR}/include/litert_lm/c/" \
    && cp -avL bazel-bin/runtime/engine/litert_lm_main \
               "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm" \
    && cp -avL bazel-bin/runtime/engine/litert_lm_advanced_main \
               "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm-advanced" \
    && echo "Staged LiteRT-LM library:" \
    && ls -lah "${LITERT_LM_DEPLOY_DIR}/lib/" \
    && test -f "${LITERT_LM_DEPLOY_DIR}/lib/liblitert_lm_c_api.so" \
    && echo "✓ liblitert_lm_c_api.so staged successfully" \
    && echo "Staged LiteRT-LM CLI tools:" \
    && ls -lah "${LITERT_LM_DEPLOY_DIR}/bin/" \
    && test -f "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm" \
    && test -f "${LITERT_LM_DEPLOY_DIR}/bin/litert-lm-advanced" \
    && echo "✓ litert-lm CLI tools staged successfully"

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
# Note: For QAIServe we KEEP the QNN and SNPE libraries, unlike the responses server.
ENV QAIRT_VERSION=${QAIRT_SDK_VERSION}
ENV QAIRT_DOWNLOAD_DIR=/tmp/qairt-download
ENV QAIRT_LIB_PATH=${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/lib

RUN mkdir -p ${QAIRT_DOWNLOAD_DIR} \
             ${DEPLOY_DIR}/usr/lib/rfsa/adsp \
             ${DEPLOY_DIR}/usr/include && \
    wget -c -t 5 -T 120 -P ${QAIRT_DOWNLOAD_DIR} \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VERSION}/v${QAIRT_VERSION}.zip && \
    cd ${QAIRT_DOWNLOAD_DIR} && \
    unzip v${QAIRT_VERSION}.zip && \
    rm -f v${QAIRT_VERSION}.zip && \
    cp -r ${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/Genie/* /usr/include/ && \
    cp -r ${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/QNN/* /usr/include/ && \
    cp -r ${QAIRT_DOWNLOAD_DIR}/qairt/${QAIRT_VERSION}/include/SNPE/* /usr/include/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so /usr/lib/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnn*.so /usr/lib/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSNPE*.so /usr/lib/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v68/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v73/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/hexagon-v75/unsigned/* ${DEPLOY_DIR}/usr/lib/rfsa/adsp/ && \
    cp ${QAIRT_LIB_PATH}/aarch64-oe-linux-gcc11.2/* ${DEPLOY_DIR}/usr/lib/ && \
    rm -rf ${QAIRT_DOWNLOAD_DIR}/qairt

# ── Stage the GenieX SDK (libgeniex.so + headers) ────────────────────────────
# GenieX model-manager C ABI, used by QAIServe's GenieXClient for multi-hub
# model downloads (HuggingFace / AI Hub / …). The SDK zip lives on the SAME
# Qualcomm public S3 mirror as QAIRT.
#   - Headers (geniex.h, geniex_model.h) → /usr/include/  (for build)
#   - libgeniex.so + libgeniex_* backends → /usr/lib/ (build) + ${DEPLOY_DIR}
# We intentionally do NOT copy the SDK's bundled lib/qairt/htp-files/* (libQnn*,
# libQnnSystem, …) — those overlap the QAIRT libs already staged above; reusing
# a single QAIRT copy avoids duplicate/mismatched QNN runtimes.
ENV GENIEX_SDK_VERSION=v0.3.14
ENV GENIEX_DOWNLOAD_DIR=/tmp/geniex-download

RUN mkdir -p ${GENIEX_DOWNLOAD_DIR} && \
    wget -t 2 -T 60 -P ${GENIEX_DOWNLOAD_DIR} \
        https://github.com/qualcomm/GenieX/releases/download/${GENIEX_SDK_VERSION}/geniex-sdk-linux-arm64-${GENIEX_SDK_VERSION}.zip  && \
    cd ${GENIEX_DOWNLOAD_DIR} && \
    unzip geniex-sdk-linux-arm64-${GENIEX_SDK_VERSION}.zip && \
    cp sdk-linux-arm64/include/*.h /usr/include/ && \
    cp sdk-linux-arm64/lib/libgeniex.so /usr/lib/ && \
    cp sdk-linux-arm64/lib/libgeniex.so ${DEPLOY_DIR}/usr/lib/ && \
    cp sdk-linux-arm64/lib/qairt/libgeniex*.so ${DEPLOY_DIR}/usr/lib/ && \
    cp sdk-linux-arm64/lib/qairt/libgeniex-proc*.so ${DEPLOY_DIR}/usr/lib/ 2>/dev/null || true && \
    rm -rf ${GENIEX_DOWNLOAD_DIR}/sdk-linux-arm64 ${GENIEX_DOWNLOAD_DIR}/*.zip

# Copy fastrpc runtime libraries into deploy staging
RUN cp /usr/lib/libcdsprpc.so* ${DEPLOY_DIR}/usr/lib/ && \
    cp /usr/lib/libadsprpc.so* ${DEPLOY_DIR}/usr/lib/

# ── Copy LiteRT libraries built from source (AArch64, with QNN NPU dispatch) ──
# Overlays on top of the QAIRT libraries so the source-built LiteRT runtime
# (liblitert_runtime_c_api.so) and dispatch/compiler-plugin .so files are
# available for both build-time linking and runtime loading.
# Headers land in /usr/include/litert/ and /usr/include/tflite/.
COPY --from=litert_builder /mnt/work/deploy/usr /usr
COPY --from=litert_builder /mnt/work/deploy/usr/lib ${DEPLOY_DIR}/usr/lib

# ── Copy LiteRT-LM libraries and CLI tools (built on top of LiteRT) ──────────
# LiteRT-LM was built with --override_repository=litert=${LITERT_SRC_DIR}
# so it uses the exact same LiteRT version as above.
# The .so files land in /usr/lib/ alongside the LiteRT runtime libraries.
# The CLI tools (litert-lm, litert-lm-advanced) land in ${DEPLOY_DIR}/usr/bin/
# for inclusion in the final image as on-device debugging/benchmarking tools.
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib /usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/lib ${DEPLOY_DIR}/usr/lib
COPY --from=litert_lm_builder /mnt/work/deploy/usr/include /usr/include
COPY --from=litert_lm_builder /mnt/work/deploy/usr/bin ${DEPLOY_DIR}/usr/bin

# ── Build llama.cpp (provides llama-server + libllama.so + libggml*.so) ───────
# Pinned to tag b5616. Shared libs (-DBUILD_SHARED_LIBS=ON) required so
# qai-forge can link against libllama/libggml at runtime.
ENV LLAMACPP_INSTALL_DIR=/build/llamacpp-install
RUN git clone --depth 1 --branch b5616 https://github.com/ggml-org/llama.cpp /build/llama.cpp && \
    cmake -S /build/llama.cpp -B /build/llama.cpp/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DLLAMA_BUILD_SERVER=ON \
        -DLLAMA_BUILD_TESTS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF \
        -DLLAMA_CURL=OFF \
        -DGGML_NATIVE=OFF \
        -DCMAKE_INSTALL_PREFIX=${LLAMACPP_INSTALL_DIR} && \
    cmake --build /build/llama.cpp/build -j$(nproc) && \
    cmake --install /build/llama.cpp/build && \
    cp ${LLAMACPP_INSTALL_DIR}/bin/llama-server ${DEPLOY_DIR}/usr/bin/ && \
    find ${LLAMACPP_INSTALL_DIR}/lib -name "*.so*" -exec cp -P {} ${DEPLOY_DIR}/usr/lib/ \; && \
    rm -rf /build/llama.cpp

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
COPY --from=builder /build/deploy/usr/bin/qaiserve /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/genai-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/genai-vlm-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/qnn-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/snpe-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/litert-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/litert-lm-inference-worker /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/litert-lm /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/litert-lm-advanced /export/usr/bin/
COPY --from=builder /build/deploy/usr/bin/llama-server /export/usr/bin/
COPY --from=builder /build/deploy/usr/lib /export/usr/lib

# Cherry-pick only the required system .so files (with symlinks preserved).
# These are libraries not already bundled via the builder deploy tree:
#   fastrpc deps:  libyaml-0-2, libbsd, libmd
#   Drogon deps:   libjsoncpp, libssl, libcrypto, libz, libuuid
#   C++ atomics:   libatomic
#   gRPC/protobuf: libgrpc++, libgrpc, libprotobuf, and their transitive deps
#                  (abseil, re2, c-ares, gpr, upb, address_sorting)
#   llama.cpp:     libgomp (OpenMP runtime required by llama-server)
# trixie-slim already ships: libc6, libstdc++6, libgcc-s1, libm
#
# Uses find to locate .so files regardless of whether Debian placed them under
# /usr/lib/aarch64-linux-gnu/, /usr/lib/, or /lib/ — all three are searched.
RUN for lib in libyaml-0 libbsd libmd libjsoncpp libssl libcrypto libz libuuid libatomic libcurl libzip libnghttp2 libnghttp3 librtmp libidn2 libssh2 libpsl libgssapi_krb5 libkrb5 libk5crypto libcom_err libkrb5support libkeyutils "libldap*" "liblber*" libbrotlidec libbrotlicommon libzstd liblzma libbz2 libsasl2 libunistring libgnutls libhogweed libnettle libgmp libp11-kit libffi libtasn1 libgrpc++ libgrpc libgpr libaddress_sorting libprotobuf libre2 libcares libupb libgomp; do \
      find /usr/lib /usr/lib/aarch64-linux-gnu /lib -name "${lib}.so*" -exec cp -dp {} /export/usr/lib/ \; ; \
    done && \
    find /usr/lib /lib -name "libabsl_*.so*" -exec cp -dp {} /export/usr/lib/ \;
# Debian packages abseil as dozens of separately-named component libraries
# (libabsl_base.so.*, libabsl_strings.so.*, libabsl_synchronization.so.*, ...)
# rather than a single libabsl.so — the plain "${lib}.so*" glob above never
# matches any of them, so it is cherry-picked separately right above.

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
# Stage 3: Runtime (debian:trixie-slim)
#
# Uses debian:trixie-slim (glibc 2.41, GCC 14 libstdc++) rather than
# distroless/cc-debian12, because the prebuilt GenieX SDK (libgeniex.so)
# requires GLIBC_2.39 + GLIBCXX_3.4.32 — newer than bookworm/distroless-12
# (glibc 2.36) can provide. trixie-slim already ships libc6/libstdc++6/libgcc-s1
# at the required versions; the stager's cherry-picked .so files overlay on top.
# ─────────────────────────────────────────────────────────────────────────────
FROM --platform=linux/arm64 debian:trixie-slim AS qaiserve-runtime

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
