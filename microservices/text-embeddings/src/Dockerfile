ARG T2E_QNP_VER=2.44.0.260225

###############################################
# Dedicated QNN SDK Downloader Stage
# Downloads and extracts the Qualcomm Neural Network (QNN) SDK.
# This stage is highly optimized for caching to prevent redundant downloads.
###############################################
FROM python:3.10-slim AS qnn_sdk_downloader

ARG T2E_QNP_VER
ENV T2E_QNP_VER=${T2E_QNP_VER}

# Install system dependencies required for QNN SDK download and unzip
RUN apt-get update                                                                              && \
    apt-get install -y --no-install-recommends                                                     \
        wget unzip ca-certificates                                                              && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Environment variables for build and deploy directories
ENV T2E_BUILD_DIR="/mnt/work/build"
ENV T2E_DEPLOY_DIR="/mnt/work/deploy"

ENV T2E_ADSP_DIR=${T2E_DEPLOY_DIR}/usr/lib/rfsa/adsp

# qnp-sdk: fetch and copy the needed headers and libraries
ENV T2E_DOWNLOAD_DIR="/mnt/work/downloads"
ENV T2E_QNN_LIB_PATH=${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/lib
ENV T2E_QNN_BIN_PATH=${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/bin
ENV T2E_QNP_SDK=v${T2E_QNP_VER}.zip

RUN wget -t 20 -T 120 -P ${T2E_DOWNLOAD_DIR}                                                       \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${T2E_QNP_VER}/${T2E_QNP_SDK} && \
    cd ${T2E_DOWNLOAD_DIR}                                                                      && \
    unzip ${T2E_QNP_SDK}                                                                        && \
    rm -f ${T2E_QNP_SDK}                                                                        && \
    cp -r ${T2E_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so /usr/lib/                    && \
    cp -r ${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/include/Genie/* /usr/include/                && \
    mkdir -p ${T2E_ADSP_DIR}                                                                    && \
    mkdir -p ${T2E_DEPLOY_DIR}/usr/bin                                                          && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${T2E_ADSP_DIR}/                              && \
    cp ${T2E_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so ${T2E_DEPLOY_DIR}/usr/lib/      && \
    cp ${T2E_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/* ${T2E_DEPLOY_DIR}/usr/lib/                && \
    cp ${T2E_QNN_BIN_PATH}/aarch64-oe-linux-gcc11.2/* ${T2E_DEPLOY_DIR}/usr/bin/

#######################
# Stage 1: Build Stage
#######################

FROM debian:trixie-slim AS builder
WORKDIR /usr/src/app

# Install system dependencies, python3, and python3-venv for builder stage
RUN apt-get update                                                                              && \
    apt-get install -y --no-install-recommends                                                     \
        build-essential g++ python3 python3-venv python3-pip python3-dev                        && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create python virtual environment
RUN python3 -m venv /venv
ENV PATH="/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip

# Install dependencies of tokenizer
RUN python3 -m pip install "sentencepiece==0.2.1"
RUN python3 -m pip install "transformers==4.57.6"
RUN python3 -m pip install "huggingface-hub==0.36.1"

# Copy source code
COPY web-api web-api
COPY pyproject.toml pyproject.toml
COPY setup.cfg setup.cfg

# Install web-api server as python package
RUN pip install --no-cache-dir .

###############################################
# Stage 2: LiteRT Builder
# Builds LiteRT from the official LiteRT source tree with Bazel and Qualcomm
# NPU/FastRPC support enabled.  Build artifacts are staged under
# /mnt/work/deploy/usr for clean extraction by the service stage.
###############################################

FROM --platform=linux/amd64 ubuntu:22.04 AS litert_builder

ARG T2E_QNP_VER
ENV T2E_QNP_VER=${T2E_QNP_VER}

RUN dpkg --add-architecture arm64                                                               && \
    sed -i 's/deb http/deb [arch=amd64] http/g' /etc/apt/sources.list                           && \
    echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ jammy main restricted universe multiverse" >> /etc/apt/sources.list && \
    echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ jammy-updates main restricted universe multiverse" >> /etc/apt/sources.list && \
    echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports/ jammy-security main restricted universe multiverse" >> /etc/apt/sources.list && \
    apt-get update                                                                              && \
    apt-get install -y --no-install-recommends                                                     \
        build-essential                                                                            \
        binutils-aarch64-linux-gnu                                                                 \
        gcc-aarch64-linux-gnu                                                                      \
        g++-aarch64-linux-gnu                                                                      \
        clang                                                                                      \
        lld                                                                                        \
        llvm                                                                                       \
        libc++-dev:arm64                                                                           \
        libc++abi-dev:arm64                                                                        \
        libunwind-14-dev:arm64                                                                     \
        g++                                                                                        \
        wget                                                                                       \
        git                                                                                        \
        unzip                                                                                      \
        zip                                                                                        \
        python3                                                                                    \
        python3-dev                                                                                \
        python3-numpy                                                                              \
        python-is-python3                                                                          \
        ca-certificates                                                                            \
        patch                                                                                      \
        pkg-config                                                                                 \
        file                                                                                       \
        default-jdk-headless                                                                    && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/*

# Copy pre-downloaded QAIRT SDK from qnn_sdk_downloader stage to bypass network fetching
COPY --from=qnn_sdk_downloader /mnt/work/downloads/qairt /mnt/work/downloads/qairt

# Configure the local QAIRT SDK path for Bazel (trailing slash is mandatory!)
ENV LITERT_QAIRT_SDK=/mnt/work/downloads/

# ✅ Use AMD64 Bazelisk (IMPORTANT)
ENV BAZELISK_VERSION=v1.28.1
RUN wget -t 3 -T 120 --no-verbose -O /usr/local/bin/bazel                                          \
        "https://github.com/bazelbuild/bazelisk/releases/download/${BAZELISK_VERSION}/bazelisk-linux-amd64" && \
    chmod +x /usr/local/bin/bazel

ENV LITERT_SRC_DIR=/mnt/work/src/litert
ENV LITERT_DEPLOY_DIR=/mnt/work/deploy/usr
# Ubuntu 22.04 ships Python 3.10 via apt — no source compilation needed.
ENV HERMETIC_PYTHON_VERSION=3.10
ENV PYTHON_BIN_PATH=/usr/bin/python3
ENV PYTHON_LIB_PATH=/usr/lib/python3/dist-packages
# Select "no" for every optional TF/LiteRT configure feature
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

# Get LiteRT.
# To prevent network disruptions (e.g. RPC failed, early EOF) during recursive
# cloning, we set robust, retry-safe Git configurations and use an explicit
# shell loop with retry logic to ensure high resilience and reproducibility.
# After successful acquisition, unavoidable source modifications are applied
# deterministically via checked-in patches (failing loudly if they do not apply).
RUN git config --global http.postBuffer 1048576000                                              && \
    git config --global http.lowSpeedLimit 0                                                    && \
    git config --global http.lowSpeedTime 999999                                                && \
    git config --global core.compression 0                                                      && \
    for i in {1..5}; do                                                                            \
        git clone --branch v2.1.5 --depth 1 https://github.com/google-ai-edge/LiteRT.git           \
                "${LITERT_SRC_DIR}" && break || {                                                  \
            echo "Clone failed, retrying in 15 seconds...";                                        \
            rm -rf "${LITERT_SRC_DIR}";                                                            \
            sleep 15;                                                                              \
        };                                                                                         \
    done                                                                                        && \
    cd "${LITERT_SRC_DIR}"                                                                      && \
    for i in {1..5}; do                                                                            \
        git submodule update --init --recursive --depth 1 && break || {                            \
            echo "Submodule update failed, retrying in 15 seconds...";                             \
            sleep 15;                                                                              \
        };                                                                                         \
    done                                                                                        && \
    if [ -f configure ]; then yes n | ./configure;                                                 \
    elif [ -f configure.py ]; then yes n | python3 configure.py;                                   \
    else echo "No configure script found; using env-based configuration";                          \
    fi

# Make LiteRT follow the QAIRT/QNN SDK version selected by this Dockerfile.
# LiteRT documentation(litert/vendors/qualcomm/doc/BUILD_INSTRUCTIONS.md)
# requires updating third_party/qairt/workspace.bzl when
# using a different QAIRT SDK version. Patch strip_prefix and url from T2E_QNP_VER.
RUN sed -i -E                                                                                      \
        -e "s|strip_prefix = \"qairt/[^\"]+\"|strip_prefix = \"qairt/${T2E_QNP_VER}\"|g"           \
        -e "s|url = \"https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/[^\"]+/v[^\"]+\\.zip\"|url = \"https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${T2E_QNP_VER}/v${T2E_QNP_VER}.zip\"|g" \
        "${LITERT_SRC_DIR}/third_party/qairt/workspace.bzl"                                     && \
    grep -nE "strip_prefix|url" "${LITERT_SRC_DIR}/third_party/qairt/workspace.bzl"

WORKDIR ${LITERT_SRC_DIR}

# Prepare Bazel cache outside /dev/shm. Docker's shared-memory mount is often
# too small for a LiteRT/TensorFlow/XLA build and can fail with ENOSPC.
# Also provide the cross GCC wrapper with an unprefixed "ld" for target links.
# Do not prepend /usr/aarch64-linux-gnu/bin to PATH globally: Bazel host tools
# are compiled with /usr/bin/gcc and must use the host assembler. Putting the
# AArch64 binutils first causes host compilation to fail with:
#   as: unrecognized option '--64'
RUN mkdir -p /bazel-cache                                                                       && \
    mkdir -p /opt/aarch64-binutils                                                              && \
    ln -sf /usr/aarch64-linux-gnu/bin/ld /opt/aarch64-binutils/ld

# ✅ Cross-compile build matching the successful internal build steps.
RUN bazel build --config=elinux_aarch64 -c opt                                                     \
    --repo_env=LITERT_QAIRT_SDK=/mnt/work/downloads/                                               \
    --copt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1                                                  \
    --cxxopt=-DLITERT_HAS_FASTRPC_SUPPORT_DEFAULT=1                                                \
    --copt=-DLITERT_ENABLE_NPU                                                                     \
    --copt=-std=c++17                                                                              \
    --cxxopt=-std=c++17                                                                            \
    --linkopt=-L/usr/lib/aarch64-linux-gnu                                                         \
    --linkopt=-L/usr/lib/llvm-14/lib                                                               \
    --linkopt=-Wl,-rpath-link,/usr/lib/aarch64-linux-gnu                                           \
    --linkopt=-Wl,-rpath-link,/usr/lib/llvm-14/lib                                                 \
    --linkopt=-Wl,--whole-archive                                                                  \
    --linkopt=-Wl,--no-whole-archive                                                               \
    //litert/c:litert_runtime_c_api_so                                                             \
    //litert/c:litert_tensor_buffer                                                                \
    //litert/runtime:tensor_buffer                                                                 \
    //litert/vendors/qualcomm/dispatch:dispatch_api_so                                             \
    //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so

# Stage LiteRT shared libraries, plugins, binaries, and public headers.
# Bazel often exposes outputs below bazel-bin as symlinks; "find -type f"
# misses those symlinked libraries. Use cquery to enumerate configured target
# outputs, copy regular files and symlinks with dereferencing, then fall back to
# scanning bazel-bin/bazel-out for shared/static libraries.
RUN mkdir -p "${LITERT_DEPLOY_DIR}/lib" "${LITERT_DEPLOY_DIR}/include"                          && \
    bazel cquery                                                                                   \
        --config=elinux_aarch64 -c opt                                                             \
        --repo_env=HERMETIC_PYTHON_VERSION=${HERMETIC_PYTHON_VERSION}                              \
        --repo_env=LITERT_QAIRT_SDK=/mnt/work/downloads/                                           \
        --output=files                                                                             \
        'set(//litert/c:litert_runtime_c_api_so //litert/c:litert_tensor_buffer //litert/runtime:tensor_buffer //litert/vendors/qualcomm/dispatch:dispatch_api_so //litert/vendors/qualcomm/compiler:qnn_compiler_plugin_so)' \
        > /tmp/litert_bazel_outputs.txt                                                         && \
    echo "Configured LiteRT Bazel outputs:"                                                     && \
    cat /tmp/litert_bazel_outputs.txt                                                           && \
    while IFS= read -r output; do                                                                  \
        if [ -e "$output" ] || [ -L "$output" ]; then                                              \
            case "$output" in                                                                      \
                *.so|*.so.*|*.a) cp -avL "$output" "${LITERT_DEPLOY_DIR}/lib/" ;;                  \
            esac;                                                                                  \
        fi;                                                                                        \
    done < /tmp/litert_bazel_outputs.txt                                                        && \
    cp -a litert "${LITERT_DEPLOY_DIR}/include/"                                                && \
    cp -a tflite "${LITERT_DEPLOY_DIR}/include/"                                                && \
    echo "Staged LiteRT libraries and tools:"                                                   && \
    ls -lah "${LITERT_DEPLOY_DIR}/lib"                                                          && \
    test "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f | wc -l)" -gt 0                 && \
    test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*litert*.so' -o -iname '*litert*.so.*' -o -iname '*LiteRt*.so' -o -iname '*LiteRt*.so.*' -o -iname '*litert*.a' -o -iname '*LiteRt*.a' \) | head -1)" && \
    test -n "$(find "${LITERT_DEPLOY_DIR}/lib" -maxdepth 1 -type f \( -iname '*dispatch*.so' -o -iname '*dispatch*.so.*' -o -iname '*qnn*plugin*.so' -o -iname '*qnn*plugin*.so.*' -o -iname '*dispatch*.a' -o -iname '*qnn*plugin*.a' \) | head -1)"

###############################################
# Stage 3: FastRPC Base Layer Builder
###############################################

FROM debian:trixie-slim AS fastrpc-builder

# Install build dependencies for fastrpc
RUN apt-get update                                                                              && \
    apt-get install -y --no-install-recommends build-essential                                  && \
    apt-get install -y git wget unzip libtool libbsd-dev libyaml-dev pkg-config m4 autoconf        \
        automake                                                                                && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/*

# Build and install fastrpc
RUN git clone --depth 1 https://github.com/quic/fastrpc.git                                     && \
    cd fastrpc                                                                                  && \
    autoreconf -is                                                                              && \
    CFLAGS='-UMACHINE_NAME_PATH -DMACHINE_NAME_PATH=\"/run/device-model\"'                         \
            ./configure --prefix=/usr --libdir=/usr/lib                                         && \
    make -j"$(nproc)"                                                                           && \
    make install

#########################
# Stage 4: Runtime Stage
#########################

FROM debian:trixie-slim AS service
WORKDIR /root/app/site-packages

# Installing Runtime dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends libatomic1 libbsd0 libmd0 libc++1 libc++abi1     && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Bake fastrpc DSP path config for all supported devices into a safe path
# that is never bind-mounted. The entrypoint copies it to
# /usr/share/qcom/conf.d/ at runtime after the bind-mount is in place.
RUN mkdir -p /etc/fastrpc                                                                       && \
    {                                                                                              \
      echo '# SPDX-License-Identifier: MIT';                                                       \
      echo '# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.';                 \
      echo 'machines:';                                                                            \
      echo '  Qualcomm Technologies, Inc. Monaco Monza addons:';                                   \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp';                              \
      echo '  Qualcomm Technologies, Inc. Addons IQ 9075 EVK:';                                    \
      echo '    DSP_LIBRARY_PATH: sa8775p/Qualcomm/SA8775P-RIDE/dsp';                              \
      echo '  Qualcomm Technologies, Inc. IQ8 8275 Pro SKU EVK:';                                  \
      echo '    DSP_LIBRARY_PATH: qcs8300/Qualcomm/QCS8300-RIDE/dsp';                              \
    } > /etc/fastrpc/hexagon-dsp-binaries.yaml

# Create directory structure for application and make it world-accessible
# Setting HOME to /tmp prevents KeyError: getpwuid() when running as arbitrary unnamed UID
ENV HOME=/tmp

# Create directory
RUN mkdir -p /iot-user/app/site-packages                                                        && \
    chmod -R 777 /iot-user

# Copy the virtual environment with web-api package installed with dependencies
COPY --from=builder /venv /venv

# Copy QNN libraries from builder stage
COPY --from=qnn_sdk_downloader /mnt/work/deploy/usr /usr

# Copy FastRPC libraries for CDSP communication
COPY --from=fastrpc-builder /usr/lib/libcdsprpc.so* /usr/lib/
COPY --from=fastrpc-builder /usr/lib/aarch64-linux-gnu/libyaml* /usr/lib/

# Install python3 as a dependency of hypercorn and SWaggerUI web api
RUN apt-get update                                                                              && \
    apt-get install -y                                                                             \
        python3 python3-pip                                                                     && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/*

# Copy LiteRT libraries built from source with external delegate support
# This overlays on top of the QNN libraries so the source-built libLiteRt.so
# (with TFLITE_ENABLE_EXTERNAL_DELEGATE=ON) takes precedence over pip version
COPY --from=litert_builder /mnt/work/deploy/usr /usr

ENV PATH=/venv/bin:$PATH
ENV LITERT_DISPATCH_DIR=/usr/lib
ENV LITERT_COMPILER_PLUGIN_DIR=/usr/lib
ENV T2E_DISPATCH_DIR=/usr/lib
ENV ADSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV CDSP_LIBRARY_PATH=/usr/lib/rfsa/adsp
ENV LD_LIBRARY_PATH=/usr/lib:$LD_LIBRARY_PATH
ENV T2E_PREFER_HTP=1
ENV T2E_REQUIRE_HTP=1

# Copy the Python entrypoint and make it executable
COPY entrypoint.py /entrypoint.py
RUN chmod +x /entrypoint.py

WORKDIR /mnt/work/models

# Container starts as root so entrypoint.py can adjust group IDs,
# then drops to iot-user before executing the application command.
ENTRYPOINT ["python3", "/entrypoint.py"]
