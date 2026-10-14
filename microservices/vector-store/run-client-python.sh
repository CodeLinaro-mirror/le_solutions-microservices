#!/usr/bin/env bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# =============================================================================
# Automated Build & Execution Shell Script for the Python Vector Store Client
# Adheres strictly to production engineering guidelines.
# =============================================================================

# Prevent script execution if any command fails in the pipeline

run_client_python() {
    # Resolve the workspace directory using the requested cd and dirname technique
    local DOCKER_DIR="$(cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

    if docker image inspect vector-store-client-python > /dev/null 2>&1; then
        echo "✅ Docker image 'vector-store-client-python' already exists. Skipping build step."
    else
        echo "============================================================================="
        echo "🔨 STEP 1: Building Standalone Python Client Image..."
        echo "============================================================================="
        docker build -f "${DOCKER_DIR}/client/Dockerfile"                                          \
            -t vector-store-client-python "${DOCKER_DIR}"
    fi

    # Create the localized data directory to hold config and document assets
    mkdir -p "${DOCKER_DIR}/data"
    if [[ -f "${DOCKER_DIR}/.env" ]]; then
        cp "${DOCKER_DIR}/.env" "${DOCKER_DIR}/data/.env"
    fi

    # Copy the client configuration file to the data folder so that the container
    # can read it natively under the /data volume mount path
    if [[ -f "${DOCKER_DIR}/client/vector_store_client_config.json" ]]; then
        cp "${DOCKER_DIR}/client/vector_store_client_config.json"                                  \
            "${DOCKER_DIR}/data/vector_store_client_config.json"
    fi

    # Resolve the input_file attribute from the config JSON using jq
    local CONFIG_FILE="${DOCKER_DIR}/client/vector_store_client_config.json"
    local INPUT_FILE=""
    if [[ -f "${CONFIG_FILE}" ]]; then
        INPUT_FILE="$(jq -r '.input_file // empty' "${CONFIG_FILE}")"
    fi

    # Sync input file if specified and present on the host
    if [[ -n "${INPUT_FILE}" && -f "${INPUT_FILE}" ]]; then
        echo "⚙️  Syncing input file: ${INPUT_FILE} -> ${DOCKER_DIR}/data/."
        rsync -aP "${INPUT_FILE}" "${DOCKER_DIR}/data/."
    fi

    echo ""
    echo "============================================================================="
    echo "🚀 STEP 2: Running Containerized Python Client on Host Network..."
    echo "============================================================================="
    # Run the client container, mounting the host data directory at /data so it can
    # dynamically access test documents, configuration files, and .env files.
    docker run --rm --network host                                                                 \
        -v "${DOCKER_DIR}/data:/data"                                                              \
        -e PYTHONPATH=/app/src                                                                     \
        vector-store-client-python                                                                 \
        python src/main.py
}

# Invoke the localized execution function
echo "run_client_python"
echo "    Run python vector-store client"
