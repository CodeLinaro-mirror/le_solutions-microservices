#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Configure the chatcompletions project
function chatcompletions-configure() {
    (
        cd ${CHATCOMPLETIONS_BUILD_DIR}                                                         && \
            cmake ${CHATCOMPLETIONS_SRC_DIR}/genai-lib
    ) || {
        print-red "Cmake chatcompletions-configure failed !!!"
        return -1
    }

    print-green "Configure chatcompletions completed successfully !"
    return 0
}

# Compile the chatcompletions project
function chatcompletions-compile() {
    (
        cd ${CHATCOMPLETIONS_BUILD_DIR}                                                         && \
            cmake --build . -j
    ) || {
        print-red "Cmake chatcompletions-compile failed !!!"
        return -1
    }

    print-green "Compile chatcompletions completed successfully !"
    return 0
}

# Install the chatcompletions project
function chatcompletions-install() {
    (
        cd ${CHATCOMPLETIONS_BUILD_DIR}                                                         && \
            cmake --install . --prefix ${CHATCOMPLETIONS_DEPLOY_DIR}/usr --strip
    ) || {
        print-red "Cmake chatcompletions-install failed !!!"
        return -1
    }

    print-green "Install chatcompletions completed successfully !"
    return 0
}

# Build the chatcompletions project
function chatcompletions-build() {

    chatcompletions-configure                                                                   && \
            chatcompletions-compile                                                             && \
            chatcompletions-install                                                             || {
        print-red "Build chatcompletions failed !!!"
        return -1
    }

    print-green "Build chatcompletions completed successfully !"
    return 0
}

# Clean the chatcompletions project
function chatcompletions-clean() {
    rm -rf ${CHATCOMPLETIONS_BUILD_DIR}/*

    print-green "chatcompletions clean up completed successfully !"
}

# Install the python openapi_server
function chatcompletions-install-openapi-server() {
    pip install ${CHATCOMPLETIONS_SRC_DIR}                                                      || {
        print-red "pip install openapi_server failed !!!"
        return -1
    }

    print-green "pip install openapi_server completed successfully !"
    return 0
}

[ -n "${CHATCOMPLETIONS_SRC_DIR}" ] && [ -d "${CHATCOMPLETIONS_SRC_DIR}" ] && {
    print-blue "chatcompletions-configure"
    echo "    Configure the chatcompletions project"
    print-blue "chatcompletions-compile"
    echo "    Compile the chatcompletions project"
    print-blue "chatcompletions-install"
    echo "    Install the chatcompletions project"
    print-blue "chatcompletions-build"
    echo "    Build the chatcompletions project"
    print-blue "chatcompletions-clean"
    echo "    Clean the chatcompletions project"
    print-blue "chatcompletions-install-openapi-server"
    echo "    Install the python openapi_server"
}
