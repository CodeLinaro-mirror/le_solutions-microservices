#######################
# Stage 1: Build Stage
#######################

FROM ubuntu:24.04 AS builder
WORKDIR /usr/src/app

# Install build dependencies for C/C++ compilation and other utilities

# Install Python 3.12 and required tools
RUN apt-get update && \
    apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    unzip build-essential cmake wget && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# create python virtual environment
RUN python3 -m venv /venv
ENV PATH="/venv/bin:$PATH"

RUN pip install --upgrade pip

# copy source code
COPY . .

# package folder name can not contain the hyphen(-) so replacing it with underscore(_)
RUN mv web-api/openapi-server web-api/openapi_server
RUN mv web-api/openapi_server/impl/genie-wrapper/ web-api/openapi_server/impl/genie_wrapper/

# install web-api server as python package
RUN pip install --no-cache-dir .


ENV LLM_BUILD_DIR="/mnt/work/build"
ENV LLM_DEPLOY_DIR="/mnt/work/deploy"

ADD . ${LLM_BUILD_DIR}/

WORKDIR ${LLM_BUILD_DIR}/

#Add LLM install directory
ENV LLM_ADSP_DIR=${LLM_DEPLOY_DIR}/usr/lib/rfsa/adsp

# qnp-sdk: fetch and copy the needed headers and libraries
ENV LLM_QNP_VER=2.39.0.250926
ENV LLM_DOWNLOAD_DIR="/mnt/work/downloads"
ENV LLM_QNN_LIB_PATH=${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/lib

ENV LLM_QNP_SDK=v${LLM_QNP_VER}.zip
RUN wget -t 2 -T 30 -P ${LLM_DOWNLOAD_DIR}                                                      \
        https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${LLM_QNP_VER}/${LLM_QNP_SDK} && \
    cd ${LLM_DOWNLOAD_DIR}                                                                     && \
    unzip ${LLM_QNP_SDK}                                                                       && \
    rm -f ${LLM_QNP_SDK}                                                                       && \
    cp -r ${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/lib/aarch64-oe-linux-gcc11.2/libGenie.so \
          /usr/lib/                                                                            && \
    cp -r ${LLM_DOWNLOAD_DIR}/qairt/${LLM_QNP_VER}/include/Genie/*                    \
          /usr/include/                                                                        && \
    mkdir -p ${LLM_ADSP_DIR}                                                                   && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v66/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v68/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v69/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v73/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v75/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/hexagon-v79/unsigned/* ${LLM_ADSP_DIR}/                             && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libGenie.so                                 \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          &&  \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/*            \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    rm -rf ${LLM_DOWNLOAD_DIR}/qairt

# Configure the project with a custom install prefix
RUN cd /usr/src/app/genai-lib && cmake . -DCMAKE_INSTALL_PREFIX=${LLM_DEPLOY_DIR}/usr

# Build the project
RUN cd /usr/src/app/genai-lib && make -j$(nproc)

# Install the project to the specified location
RUN cd /usr/src/app/genai-lib && make install

#########################
# Stage 2: Runtime Stage
#########################

FROM ubuntu:24.04 AS service
WORKDIR /root/app/site-packages

# Install Python 3.12 and required tools
RUN apt-get update && \
    apt-get install -y \
    python3 && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* /var/tmp/*

# create directory
RUN mkdir -p /root/app/site-packages

# copy the virtual environment with web-api package installed with dependencies
COPY --from=builder /venv /venv

# copy interface file
COPY --from=builder /usr/src/app/web-api/openapi_server/impl/genie_wrapper/include/genai_interface.h /usr/include/

COPY --from=builder /usr/src/app/web-api/openapi_server /usr/src/app/web-api

# copy all libraries
COPY --from=builder /mnt/work/deploy/usr /usr

# create symlink of libllmservice library
RUN ln -s /usr/lib/libllmservice.so /root/app/site-packages/libllmservice.so

ENV PATH=/venv/bin:$PATH
