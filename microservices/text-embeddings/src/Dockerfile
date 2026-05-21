#######################
# Stage 1: Build Stage
#######################

FROM python:3.10-slim AS builder
WORKDIR /usr/src/app

# Install system dependencies required for Qualcomm SDK and other tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++ wget unzip && \
    apt-get clean && rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create python virtual environment
RUN python -m venv /venv
ENV PATH="/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip

# Environment variables for build and deploy directories
ENV T2E_BUILD_DIR="/mnt/work/build"
ENV T2E_DEPLOY_DIR="/mnt/work/deploy"

ENV T2E_ADSP_DIR=${T2E_DEPLOY_DIR}/usr/lib/rfsa/adsp

# qnp-sdk: fetch and copy the needed headers and libraries
ENV T2E_QNP_VER=2.42.0.251225
ENV T2E_DOWNLOAD_DIR="/mnt/work/downloads"
ENV T2E_QNN_LIB_PATH=${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/lib
ENV T2E_QNN_BIN_PATH=${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/bin
ENV T2E_QNP_SDK=v${T2E_QNP_VER}.zip

RUN wget -t 2 -T 30 -P ${T2E_DOWNLOAD_DIR} \
    https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${T2E_QNP_VER}/${T2E_QNP_SDK} && \
    cd ${T2E_DOWNLOAD_DIR} && \
    unzip ${T2E_QNP_SDK} && \
    rm -f ${T2E_QNP_SDK} && \
    cp -r ${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/lib/aarch64-oe-linux-gcc11.2/libGenie.so /usr/lib/ && \
    cp -r ${T2E_DOWNLOAD_DIR}/qairt/${T2E_QNP_VER}/include/Genie/* /usr/include/ && \
    mkdir -p ${T2E_ADSP_DIR} && \
    mkdir -p ${T2E_DEPLOY_DIR}/usr/bin && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${T2E_ADSP_DIR}/ && \
    cp ${T2E_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so ${T2E_DEPLOY_DIR}/usr/lib/ && \
    cp ${T2E_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/* ${T2E_DEPLOY_DIR}/usr/lib/ && \
    cp ${T2E_QNN_BIN_PATH}/aarch64-oe-linux-gcc11.2/* ${T2E_DEPLOY_DIR}/usr/bin/ && \
    rm -rf ${T2E_DOWNLOAD_DIR}/qairt

# Install ai-edge-litert to get libLiteRt.so
RUN python3 -m pip install "ai-edge-litert==2.1.2"

# Install dependencies of tokenizer
RUN python3 -m pip install "sentencepiece==0.2.1"
RUN python3 -m pip install "transformers==4.57.6"
RUN python3 -m pip install "huggingface-hub==0.36.1"

# Copy source code
COPY . .

# Install web-api server as python package
RUN pip install --no-cache-dir .

###############################################
# Stage 2: FastRPC Base Layer Builder
###############################################

FROM debian:trixie-slim AS fastrpc-builder

# Install build dependencies for fastrpc
RUN apt-get update                                                                              && \
    apt-get install -y --no-install-recommends build-essential                                  && \
    apt-get install -y git wget unzip libtool libbsd-dev libyaml-dev pkg-config m4 autoconf        \
            automake                                                                            && \
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
# Stage 3: Runtime Stage
#########################

FROM debian:trixie-slim AS service
WORKDIR /root/app/site-packages

#Installing Runtime dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends libatomic1 libbsd0 libmd0                        && \
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

# Copy all libraries
COPY --from=builder /mnt/work/deploy/usr /usr
COPY --from=fastrpc-builder /usr/lib/libcdsprpc.so* /usr/lib/
COPY --from=fastrpc-builder /usr/lib/aarch64-linux-gnu/libyaml* /usr/lib/

# Install python3 as a dependency of hypercorn and SWaggerUI web api
RUN apt-get update && apt-get install -y python3 python3-pip && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Copy installed site-packages from builder
COPY --from=builder /usr/local /usr/local

# Copy the libLiteRT.so, dependency of LiteRT python API, to /usr/lib
RUN cp /venv/lib/python3.10/site-packages/ai_edge_litert/libLiteRt.so /usr/lib/.

ENV PATH=/venv/bin:$PATH

# Copy the Python entrypoint and make it executable
COPY entrypoint.py /entrypoint.py
RUN chmod +x /entrypoint.py

# Container starts as root so entrypoint.py can adjust group IDs,
# then drops to iot-user before executing the application command.
ENTRYPOINT ["python3", "/entrypoint.py"]
