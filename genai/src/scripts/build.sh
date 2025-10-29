#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Configure the LLM project
function LLM-configure() {
    (
        cd ${LLM_BUILD_DIR}                                                                     && \
            cmake ${LLM_SRC_DIR}/genai-lib
    ) || {
        print-red "Cmake LLM-configure failed !!!"
        return -1
    }

    print-green "Configure LLM completed successfully !"
    return 0
}

# Configure the VLM project
function VLM-configure() {
    (
        cd ${VLM_BUILD_DIR}                                                                     && \
            cmake ${VLM_SRC_DIR}/genai-lib
    ) || {
        print-red "Cmake VLM-configure failed !!!"
        return -1
    }

    print-green "Configure VLM completed successfully !"
    return 0
}

# Compile the LLM project
function LLM-compile() {
    (
        cd ${LLM_BUILD_DIR}                                                                     && \
            cmake --build . -j
    ) || {
        print-red "Cmake LLM-compile failed !!!"
        return -1
    }

    print-green "Compile LLM completed successfully !"
    return 0
}

# Compile the VLM project
function VLM-compile() {
    (
        cd ${VLM_BUILD_DIR}                                                                     && \
            cmake --build . -j
    ) || {
        print-red "Cmake VLM-compile failed !!!"
        return -1
    }

    print-green "Compile VLM completed successfully !"
    return 0
}

# Install the LLM project
function LLM-install() {
    (
        cd ${LLM_BUILD_DIR}                                                                     && \
            cmake --install . --prefix ${LLM_DEPLOY_DIR}/usr --strip
    ) || {
        print-red "Cmake LLM-install failed !!!"
        return -1
    }

    print-green "Install LLM completed successfully !"
    return 0
}

# Install the VLM project
function VLM-install() {
    (
        cd ${VLM_BUILD_DIR}                                                                     && \
            cmake --install . --prefix ${VLM_DEPLOY_DIR}/usr --strip
    ) || {
        print-red "Cmake VLM-install failed !!!"
        return -1
    }

    print-green "Install VLM completed successfully !"
    return 0
}

# Build the LLM project
function LLM-build() {

    LLM-configure                                                                               && \
            LLM-compile                                                                         && \
            LLM-install                                                                         || {
        print-red "Build LLM failed !!!"
        return -1
    }

    print-green "Build LLM completed successfully !"
    return 0
}

# Build the VLM project
function VLM-build() {

    VLM-configure                                                                               && \
            VLM-compile                                                                         && \
            VLM-install                                                                         || {
        print-red "Build VLM failed !!!"
        return -1
    }

    print-green "Build VLM completed successfully !"
    return 0
}

# Clean the LLM project
function LLM-clean() {
    rm -rf ${LLM_BUILD_DIR}/*

    print-green "LLM clean up completed successfully !"
}

# Clean the VLM project
function VLM-clean() {
    rm -rf ${VLM_BUILD_DIR}/*

    print-green "VLM clean up completed successfully !"
}

# Install the python openapi_server
function VLM-install-openapi-server() {
    pip install ${VLM_SRC_DIR}                                                                  || {
        print-red "pip install openapi_server failed !!!"
        return -1
    }

    print-green "pip install openapi_server completed successfully !"
    return 0
}

[ -n "${LLM_SRC_DIR}" ] && [ -d "${LLM_SRC_DIR}" ] && {
    print-blue "LLM-configure"
    echo "    Configure the LLM project"
    print-blue "LLM-compile"
    echo "    Compile the LLM project"
    print-blue "LLM-install"
    echo "    Install the LLM project"
    print-blue "LLM-build"
    echo "    Build the LLM project"
    print-blue "LLM-clean"
    echo "    Clean the LLM project"
}

[ -n "${VLM_SRC_DIR}" ] && [ -d "${VLM_SRC_DIR}" ] && {
    print-blue "VLM-configure"
    echo "    Configure the VLM project"
    print-blue "VLM-compile"
    echo "    Compile the VLM project"
    print-blue "VLM-install"
    echo "    Install the VLM project"
    print-blue "VLM-build"
    echo "    Build the VLM project"
    print-blue "VLM-clean"
    echo "    Clean the VLM project"
    print-blue "VLM-install-openapi-server"
    echo "    Install the python openapi_server"
}
