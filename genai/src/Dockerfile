#######################
# Stage 1: Build Stage
#######################

# Use ubuntu 24.04 as base image
FROM ubuntu:24.04 AS genai-builder

# Enable color prompt
RUN sed -i 's/#force_color_prompt=yes/force_color_prompt=yes/g' /root/.bashrc

# Enable multiarch
RUN dpkg --add-architecture arm64

# Adjust apt sources lists for x86 packages
RUN sed -i 's|http://ports.ubuntu.com/ubuntu-ports/|http://archive.ubuntu.com/ubuntu/|'            \
        /etc/apt/sources.list.d/ubuntu.sources                                                  && \
    sed -i '/^Types: deb$/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources        && \
    sed -i '0,/http:\/\/archive.ubuntu.com\/ubuntu\//s|http://archive.ubuntu.com/ubuntu/|http://security.ubuntu.com/ubuntu/|' \
        /etc/apt/sources.list.d/ubuntu.sources

# Duplicate for arm64
RUN cp /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/arm64.sources

# Adjust apt sources lists for arm packages
RUN sed -i '/^Architectures:/d' /etc/apt/sources.list.d/arm64.sources                           && \
    sed -i '/^Types: deb$/a Architectures: arm64' /etc/apt/sources.list.d/arm64.sources         && \
    sed -i 's|http://archive.ubuntu.com/ubuntu/|http://ports.ubuntu.com/ubuntu-ports/|g'           \
        /etc/apt/sources.list.d/arm64.sources                                                   && \
    sed -i 's|http://security.ubuntu.com/ubuntu/|http://ports.ubuntu.com/ubuntu-ports/|g'          \
         /etc/apt/sources.list.d/arm64.sources

# Install Python 3.12 and required tools
RUN apt-get update                                                                              && \
    apt-get install -y                                                                             \
        python3                                                                                    \
        python3-pip                                                                                \
        python3-venv                                                                               \
        gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libjsoncpp-dev:arm64 pkg-config                \
        unzip cmake wget                                                                        && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Create python virtual environment
RUN python3 -m venv /mnt/work/venv
ENV PATH="/mnt/work/venv/bin:$PATH"
RUN pip install --upgrade pip

# Add needed environment variables
ENV TARGET_SYSROOT="/"
ENV LLM_SRC_DIR="/mnt/work/src"
ENV LLM_BUILD_DIR="/mnt/work/build"
ENV LLM_DEPLOY_DIR="/mnt/work/deploy"
ENV LLM_DOWNLOAD_DIR="/mnt/work/downloads"
ENV LLM_SCRIPTS_DIR="/mnt/work/scripts"
ENV LLM_ADSP_DIR=${LLM_DEPLOY_DIR}/usr/lib/rfsa/adsp
RUN mkdir -p ${LLM_BUILD_DIR} ${TARGET_SYSROOT}/usr/include ${TARGET_SYSROOT}/usr/lib

# qnp-sdk: fetch and copy the needed headers and libraries
ENV LLM_QNP_VER=2.39.0.250926
ENV LLM_QNN_LIB_PATH=${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/lib

ENV LLM_QNP_SDK=v${LLM_QNP_VER}.zip
RUN wget -t 2 -T 30 -P ${LLM_DOWNLOAD_DIR}                                                         \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${LLM_QNP_VER}/${LLM_QNP_SDK} && \
    cd ${LLM_DOWNLOAD_DIR}                                                                      && \
    unzip ${LLM_QNP_SDK}                                                                        && \
    rm -f ${LLM_QNP_SDK}                                                                        && \
    cp -r ${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/lib/aarch64-oe-linux-gcc11.2/libGenie.so        \
          /usr/lib/                                                                             && \
    cp -r ${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/include/Genie/*                                 \
          /usr/include/                                                                         && \
    mkdir -p ${LLM_ADSP_DIR}                                                                    && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${LLM_ADSP_DIR}/                              && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so                                    \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                             &&  \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/*                                              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                              && \
    rm -rf ${LLM_DOWNLOAD_DIR}/qairt

# Add root to plugdev group
RUN usermod -aG plugdev root

# Copy web-api source code
COPY setup.cfg ${LLM_SRC_DIR}/setup.cfg
COPY pyproject.toml ${LLM_SRC_DIR}/pyproject.toml
COPY web-api ${LLM_SRC_DIR}/web-api
COPY README.md ${LLM_SRC_DIR}/README.md

# Install web-api server as python package
RUN pip install ${LLM_SRC_DIR}

# Copy helper scripts
COPY scripts ${LLM_SCRIPTS_DIR}

# Copy genai-lib source code
COPY genai-lib ${LLM_SRC_DIR}/genai-lib

# Add bash aliases file
COPY .bash_aliases /root/.bash_aliases

# Build the LLM project
RUN bash -c "source ${VLM_SCRIPTS_DIR}/setup.sh && LLM-build"

#########################
# Stage 2: Runtime Stage
#########################

FROM ubuntu:24.04 AS genai-service

# Create USER
RUN groupadd qcom                                                                               && \
    useradd -s /bin/bash -m -g qcom genai                                                       && \
    echo genai:asd | chpasswd

# Install Python 3.12 and required tools
RUN apt-get update                                                                              && \
    apt-get install -y                                                                             \
        python3 libjsoncpp25 libatomic1                                                         && \
    apt-get clean                                                                               && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# Copy the virtual environment with web-api package installed with dependencies
COPY --from=genai-builder /mnt/work/venv /mnt/work/venv
ENV PATH=/mnt/work/venv/bin:$PATH

# Install numpy as a dependency of PreprocessedImage
RUN pip install numpy

# Copy interface file
COPY --from=genai-builder                                                                          \
    /mnt/work/src/web-api/openapi_server/impl/genie_wrapper/include/genai_interface.h              \
    /usr/include/

# Copy all libraries
COPY --from=genai-builder /mnt/work/deploy/usr /usr

# Copy openapi.yaml as a dependency of FastAPI's openapi schema
COPY --from=genai-builder /mnt/work/src/web-api/openapi_server/openapi.yaml /

# Change user to genai
USER genai
