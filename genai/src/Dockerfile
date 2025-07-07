#######################
# Stage 1: Build Stage
#######################

FROM python:3.10.12 AS builder
WORKDIR /usr/src/app

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

# Enable color prompt
RUN sed -i 's/#force_color_prompt=yes/force_color_prompt=yes/g' /root/.bashrc

ENV LLM_BUILD_DIR="/mnt/work/build"
ENV LLM_DEPLOY_DIR="/mnt/work/deploy"

ADD . ${LLM_BUILD_DIR}/

WORKDIR ${LLM_BUILD_DIR}/

RUN apt-get update                                                                              && \
        DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get install -y                               \
        git tar unzip bash-completion build-essential cmake wget                                && \
        apt-get autoremove -y                                                                   && \
        apt-get clean                                                                           && \
        rm -rf /var/lib/apt/lists* /var/tmp/*

#Add LLM install directory
ENV LLM_ADSP_DIR=${LLM_DEPLOY_DIR}/usr/lib/rfsa/adsp

# qnp-sdk: fetch and copy the needed headers and libraries
ENV LLM_QNP_VER=2.34.0.250424
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
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libhta_hexagon_runtime_snpe.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libPlatformValidatorShared.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnChrometraceProfilingReader.so         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnCpu.so                                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnDspNetRunExtensions.so                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnDsp.so                                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnDspV66CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnDspV66Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGenAiTransformerCpuOpPkg.so           \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGenAiTransformerModel.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGenAiTransformer.so                   \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGpuNetRunExtensions.so                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGpuProfilingReader.so                 \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnGpu.so                                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpNetRunExtensions.so                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpOptraceProfilingReader.so          \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpPrepare.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpProfilingReader.so                 \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtp.so                                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV68CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV68Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV69CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV69Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV73CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV73Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV75CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV75Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV79CalculatorStub.so               \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnHtpV79Stub.so                         \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnIr.so                                 \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnJsonProfilingReader.so                \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnModelDlc.so                           \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnSaver.so                              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnSystem.so                             \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libQnnTFLiteDelegate.so                     \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeDspV66Stub.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpPrepare.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV68CalculatorStub.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV68Stub.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV73CalculatorStub.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV73Stub.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV75CalculatorStub.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV75Stub.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV79CalculatorStub.so              \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSnpeHtpV79Stub.so                        \
        ${LLM_DEPLOY_DIR}/usr/lib/                                                          && \
    cp ${LLM_QNN_LIB_PATH}/aarch64-oe-linux-gcc11.2/libSNPE.so                                  \
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

FROM python:3.10.12 AS service
WORKDIR /root/app/site-packages

# create directory
RUN mkdir -p /root/app/site-packages

# copy the virtual environment with web-api package installed with dependencies
COPY --from=builder /venv /venv
# copy interface file
COPY --from=builder /usr/src/app/web-api/openapi_server/impl/genie_wrapper/include/genai_interface.h /usr/include/

# copy all libraries 
COPY --from=builder /mnt/work/deploy/usr /usr
# create symlink of libllmservice library 
RUN ln -s /usr/lib/libllmservice.so /root/app/site-packages/libllmservice.so

ENV PATH=/venv/bin:$PATH

