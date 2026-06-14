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

#########################
# Stage 2: Runtime Stage
#########################

FROM python:3.10-slim AS service
WORKDIR /root/app/site-packages

#Installing Runtime dependencies
RUN apt-get update && \
    apt-get install -y --no-install-recommends libatomic1 && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create directory
RUN mkdir -p /root/app/site-packages

# Copy the virtual environment with web-api package installed with dependencies
COPY --from=builder /venv /venv

# Copy all libraries
COPY --from=builder /mnt/work/deploy/usr /usr

# Copy the libLiteRT.so, dependency of LiteRT python API, to /usr/lib
RUN cp /venv/lib/python3.10/site-packages/ai_edge_litert/libLiteRt.so /usr/lib/.

ENV PATH=/venv/bin:$PATH
