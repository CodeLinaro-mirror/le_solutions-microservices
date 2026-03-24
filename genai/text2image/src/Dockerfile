#######################
# Stage 1: Build Stage
#######################

FROM python:3.10-slim AS builder

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
ENV T2I_QNP_VER=2.40.0.251030
ENV T2I_DOWNLOAD_DIR="/mnt/work/downloads"
ENV T2I_QNN_LIB_PATH=${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/lib
ENV T2I_QNN_BIN_PATH=${T2I_DOWNLOAD_DIR}/qairt/${T2I_QNP_VER}/bin
ENV T2I_QNP_SDK=v${T2I_QNP_VER}.zip

RUN wget -t 2 -T 30 -P ${T2I_DOWNLOAD_DIR}                                                         \
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

#########################
# Stage 2: Runtime Stage
#########################

FROM python:3.10-slim AS service

# Installing Runtime dependencies
# Install Python 3.12, runtime tools needed by entrypoint.sh
# (stat/chown from coreutils, groupmod/usermod/runuser from
# passwd and util-linux, getent from libc-bin)
RUN apt-get update && \
    apt-get install -y --no-install-recommends libatomic1 coreutils passwd util-linux libc-bin  && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create groups for fastrpc and dmaheap with placeholder GIDs.
# The real GIDs are set at container startup by entrypoint.sh.
RUN groupadd --gid 1500 fastrpc                                                                 && \
    groupadd --gid 1501 dmaheap

# Create iot-user and add to the placeholder groups
RUN useradd --create-home --shell /bin/bash                                                        \
        --groups fastrpc,dmaheap iot-user

# Create directory
RUN mkdir -p /root/app/site-packages

# Copy the virtual environment with web-api package installed with dependencies
COPY --from=builder /mnt/work/venv /mnt/work/venv

# Copy all libraries
COPY --from=builder /mnt/work/deploy/usr /usr

ENV PATH=/mnt/work/venv/bin:$PATH

# Copy entrypoint script and make it executable
COPY --chmod=755 entrypoint.sh /entrypoint.sh

# Container starts as root so entrypoint.sh can adjust group IDs,
# then drops to iot-user before executing the application command.
ENTRYPOINT ["/entrypoint.sh"]
