#######################
# Stage 1: Build Stage
#######################

FROM python:3.13-slim AS builder

# Install system dependencies required for Qualcomm SDK and other tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential g++ wget unzip && \
    apt-get clean && rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create python virtual environment
RUN python -m venv /mnt/work/venv
ENV PATH="/mnt/work/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip

# Environment variables for build and deploy directories
ENV T2I_SRC_DIR="/mnt/work/src"
ENV T2I_DEPLOY_DIR="/mnt/work/deploy"
ENV T2I_ADSP_DIR=${T2I_DEPLOY_DIR}/usr/lib/rfsa/adsp

# qnp-sdk: fetch and copy the needed headers and libraries
ENV T2I_QNP_VER=2.45.40.260406
ENV T2I_DOWNLOAD_DIR="/mnt/work/downloads"
ENV T2I_QNN_LIB_PATH=${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/lib
ENV T2I_QNN_BIN_PATH=${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/bin
ENV T2I_QNP_SDK=v${T2I_QNP_VER}.zip

RUN wget -t 2 -T 30 --no-check-certificate -P ${T2I_DOWNLOAD_DIR}                                  \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${T2I_QNP_VER}/${T2I_QNP_SDK} && \
    cd ${T2I_DOWNLOAD_DIR}                                                                      && \
    unzip ${T2I_QNP_SDK}                                                                        && \
    rm -f ${T2I_QNP_SDK}                                                                        && \
    cp -r ${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/lib/aarch64-oe-linux-gcc11.2/libGenie.so        \
        /usr/lib/                                                                               && \
    cp -r ${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/include/Genie/*                                 \
        /usr/include/                                                                           && \
    mkdir -p ${T2I_ADSP_DIR}                                                                    && \
    mkdir -p ${T2I_DEPLOY_DIR}/usr/bin                                                          && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${T2I_ADSP_DIR}/                              && \
    cp ${T2I_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so ${T2I_DEPLOY_DIR}/usr/lib/      && \
    cp ${T2I_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/* ${T2I_DEPLOY_DIR}/usr/lib/                && \
    cp ${T2I_QNN_BIN_PATH}/aarch64-oe-linux-gcc11.2/* ${T2I_DEPLOY_DIR}/usr/bin/                && \
    rm -rf ${T2I_DOWNLOAD_DIR}/qairt

# Copy source code
COPY setup.cfg ${T2I_SRC_DIR}/setup.cfg
COPY pyproject.toml ${T2I_SRC_DIR}/pyproject.toml
COPY web-api ${T2I_SRC_DIR}/web-api

# Install web-api server as python package
RUN pip install ${T2I_SRC_DIR}

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

# Installing Runtime dependencies
# Install Python 3.12, runtime tools needed by entrypoint.sh
# (stat/chown from coreutils, groupmod/usermod/runuser from
# passwd and util-linux, getent from libc-bin)
RUN apt-get update && \
    apt-get install -y --no-install-recommends libatomic1 libbsd0 libmd0                       && \
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
COPY --from=builder /mnt/work/venv /mnt/work/venv

# Copy all libraries
COPY --from=builder /mnt/work/deploy/usr /usr
COPY --from=fastrpc-builder /usr/lib/libcdsprpc.so* /usr/lib/
COPY --from=fastrpc-builder /usr/lib/aarch64-linux-gnu/libyaml* /usr/lib/

# Install python3 as a dependency of hypercorn and SWaggerUI web api
RUN apt-get update && apt-get install -y python3 python3-pip && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Copy installed site-packages from builder
COPY --from=builder /usr/local /usr/local

ENV PATH=/mnt/work/venv/bin:$PATH

# Copy the Python entrypoint and make it executable
COPY entrypoint.py /entrypoint.py
RUN chmod +x /entrypoint.py

WORKDIR /mnt/work/models

# Container starts as root so entrypoint.sh can adjust group IDs,
# then drops to iot-user before executing the application command.
ENTRYPOINT ["python3", "/entrypoint.py"]
