# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Build argument to control native library installation
ARG INSTALL_NATIVE_LIBS=false
ARG UBUNTU_VERSION=24.04

# ============================
# Stage: Native Libraries (ARM64 only)
# ============================
FROM docker-registry.qualcomm.com/library/ubuntu:${UBUNTU_VERSION} AS native-libs

ARG INSTALL_NATIVE_LIBS

RUN if [ "$INSTALL_NATIVE_LIBS" = "true" ]; then \
    apt-get update && \
    apt-get install -y wget unzip cmake && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*; \
    fi

ENV VLM_BUILD_DIR="/mnt/work/build"
ENV VLM_DEPLOY_DIR="/mnt/work/deploy"
ENV VLM_ADSP_DIR=${VLM_DEPLOY_DIR}/usr/lib/rfsa/adsp
ENV VLM_QNP_VER=2.39.0.250925
ENV VLM_DOWNLOAD_DIR="/mnt/work/downloads"
ENV VLM_QNN_LIB_PATH=${VLM_DOWNLOAD_DIR}/qairt/${VLM_QNP_VER}/lib
ENV VLM_QNP_SDK=v${VLM_QNP_VER}.zip

# Always create directory structure so COPY commands don't fail
RUN mkdir -p ${VLM_DEPLOY_DIR}/usr/lib

RUN if [ "$INSTALL_NATIVE_LIBS" = "true" ]; then \
    wget -t 3 -T 300 -P ${VLM_DOWNLOAD_DIR} \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${VLM_QNP_VER}/${VLM_QNP_SDK} && \
    cd ${VLM_DOWNLOAD_DIR} && \
    unzip ${VLM_QNP_SDK} && \
    rm -f ${VLM_QNP_SDK} && \
    cp -r ${VLM_DOWNLOAD_DIR}/qairt/${VLM_QNP_VER}/lib/aarch64-oe-linux-gcc11.2/libGenie.so \
          /usr/lib/ && \
    cp -r ${VLM_DOWNLOAD_DIR}/qairt/${VLM_QNP_VER}/include/Genie/* \
          /usr/include/ && \
    mkdir -p ${VLM_ADSP_DIR} && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${VLM_ADSP_DIR}/ && \
    cp ${VLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so \
        ${VLM_DEPLOY_DIR}/usr/lib/ && \
    cp ${VLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/* \
        ${VLM_DEPLOY_DIR}/usr/lib/ && \
    rm -rf ${VLM_DOWNLOAD_DIR}/qairt; \
    fi

# ============================
# Stage: Runtime
# ============================
FROM docker-registry.qualcomm.com/library/ubuntu:${UBUNTU_VERSION} AS runtime

ARG INSTALL_NATIVE_LIBS

# Install system dependencies including PortAudio and ALSA for sounddevice
RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y \
    python3 \
    python3-pip \
    python3-dev \
    build-essential \
    curl \
    portaudio19-dev \
    libportaudio2 \
    libasound2-dev \
    libasound2t64 \
    && rm -rf /var/lib/apt/lists/*

# Create app user
RUN useradd -ms /bin/bash audioserver
USER audioserver
WORKDIR /usr/src/app

# Copy requirements first for better caching
COPY --chown=audioserver:audioserver app/requirements.txt ./

# Install Python dependencies globally
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt && \
    python3 -c "import site; print('Python packages installed at:', site.getsitepackages())"

# Copy application code
COPY --chown=audioserver:audioserver app/ ./

# Copy Python wrapper files and models config
USER root
RUN mkdir -p /usr/src/engine/python /opt/audio/models
COPY --chown=audioserver:audioserver engine/python/*.py /usr/src/engine/python/
COPY --chown=audioserver:audioserver engine/config/models.json /opt/audio/models/models.json

# Copy native libraries (directory always exists, only populated when INSTALL_NATIVE_LIBS=true)
COPY --from=native-libs /mnt/work/deploy/usr /usr

USER audioserver

# Create directories for models and cache
RUN mkdir -p /home/audioserver/models /home/audioserver/cache

# Set Python path
ENV PYTHONPATH=/usr/src/app
# Using system Python packages

# Set environment variables for native libraries (used when host mounts libs)
ENV DSP_LIBRARY_PATH=/usr/src/app
ENV LD_LIBRARY_PATH=/usr/src/app:/usr/lib

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=40s --retries=3 \
    CMD python3 -c "import redis; r=redis.Redis(host='${REDIS_HOST:-redis}', port=${REDIS_PORT:-6379}); r.ping()" || exit 1

# Run the application
CMD ["python3", "main.py"]
