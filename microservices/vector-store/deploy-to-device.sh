#!/usr/bin/env bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# =============================================================================
# deploy-to-device.sh
#
# Build linux/arm64 Docker images on this PC, transfer them to an ARM64
# device via ADB, and start the full stack on the device.
#
# =============================================================================

# -- Helpers -------------------------------------------------------------------
log()  { echo "==> $*"; }
ok()   { echo "    ✓ $*"; }
die()  { echo "ERROR: $*" >&2; return -1; }

deploy_to_device() {
    # -- Local Configuration ---------------------------------------------------
    # Resolve configuration file path relative to the script directory
    local SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    local CONFIG_FILE="${SCRIPT_DIR}/deploy_config.json"
    local CLIENT_CONFIG="${SCRIPT_DIR}/client/vector_store_client_config.json"

    # Default fallback values
    local JSON_DEVICE_ID
    local MAX_CPU_CORES
    local SERVER_PORT=9005
    local SWAGGER_PORT=8081
    local POSTGRES_PORT=5432

    # Read config values from json using jq if the configuration file is present
    if [[ -f "${CONFIG_FILE}" ]]; then
        JSON_DEVICE_ID="$(jq -r '.DEVICE_ID // empty' "${CONFIG_FILE}")"

        local PARSED_CORES
        PARSED_CORES="$(jq -r '.MAX_CPU_CORES // empty' "${CONFIG_FILE}")"

        # Validate that the parsed CPU cores is a valid positive integer (number)
        if [[ "${PARSED_CORES}" =~ ^[0-9]+$ ]]; then
            MAX_CPU_CORES="${PARSED_CORES}"
        else
            log "Warning: Parsed MAX_CPU_CORES ('${PARSED_CORES}') is not a valid number."
        fi
    fi

    # Read port forwarding mappings from the client configuration JSON
    if [[ -f "${CLIENT_CONFIG}" ]]; then
        SERVER_PORT="$(jq -r '.server_port // 9005' "${CLIENT_CONFIG}")"
        SWAGGER_PORT="$(jq -r '.swagger_port // 8081' "${CLIENT_CONFIG}")"
        POSTGRES_PORT="$(jq -r '.postgres_port // 5432' "${CLIENT_CONFIG}")"
    fi

    # Safely handle default device serial, prioritizing the ADB_DEVICE env override
    local DEVICE_ID="${ADB_DEVICE:-${JSON_DEVICE_ID}}"
    local ADB="adb -s ${DEVICE_ID}"
    local TMP_DIR
    TMP_DIR="$(mktemp -d)"
    local DEVICE_DIR
    local HOME_DIR
    local IMG
    local SAFE_NAME
    local OUT
    local F
    local FNAME

    # Register trap to clean up the temporary directory on function/script exit
    trap 'rm -rf "${TMP_DIR}"' EXIT

    # Images that are built locally (must match docker-compose.yml image: tags)
    local BUILT_IMAGES=(
        "vector-store:latest"
    )

    # Public images that the device can pull directly if it has internet access.
    # Set TRANSFER_PUBLIC_IMAGES=1 to transfer them via ADB instead.
    local PUBLIC_IMAGES=(
        "pgvector/pgvector:pg16"
        "swaggerapi/swagger-ui:latest"
    )
    local TRANSFER_PUBLIC_IMAGES="${TRANSFER_PUBLIC_IMAGES:-0}"

    # Dynamically select destination directory based on target OS (Linux $HOME)
    if ${ADB} shell "[ -d /data/local/tmp ]" >/dev/null 2>&1; then
        DEVICE_DIR="/data/local/tmp/vector-store"
    else
        HOME_DIR=$(${ADB} shell "echo \$HOME" | tr -d '\r')
        DEVICE_DIR="${HOME_DIR}/vector-store"
    fi

    # -- Pre-flight checks ---------------------------------------------------------
    log "Checking ADB connection to device ${DEVICE_ID}..."
    ${ADB} get-state > /dev/null 2>&1 || die "Device ${DEVICE_ID} not found. Check USB/WiFi ADB connection."
    ok "Device reachable"

    # Detect the build architecture of this PC
    local BUILDARCH="amd64"
    if [[ "$(uname -m)" == "aarch64" || "$(uname -m)" == "arm64" ]]; then
        BUILDARCH="arm64"
    fi

    local TARGETARCH="linux/arm64"

    # -- Step 1: Build linux/arm64 images on PC ------------------------------------
    log "[1/6] Building linux/arm64 images on PC..."
    docker compose build                                                                           \
        --build-arg BUILDARCH="${BUILDARCH}"                                                       \
        --build-arg TARGETARCH="${TARGETARCH}"                                                     \
        --build-arg MAX_CPU_CORES_ARG="${MAX_CPU_CORES}"
    ok "Build complete"

    # -- Step 2: Save built images to compressed tarballs -------------------------
    log "[2/6] Saving built images to ${TMP_DIR}..."
    for IMG in "${BUILT_IMAGES[@]}"; do
        SAFE_NAME="${IMG//:/_}"
        SAFE_NAME="${SAFE_NAME//\//_}"
        OUT="${TMP_DIR}/${SAFE_NAME}.tar.gz"
        log "  Saving ${IMG} → ${OUT}"
        docker save "${IMG}" | gzip > "${OUT}"
        ok "${IMG} saved ($(du -sh "${OUT}" | cut -f1))"
    done

    # Optionally save public images too (skip if device has internet)
    if [[ "${TRANSFER_PUBLIC_IMAGES}" == "1" ]]; then
        log "  Saving public images (TRANSFER_PUBLIC_IMAGES=1)..."
        for IMG in "${PUBLIC_IMAGES[@]}"; do
            SAFE_NAME="${IMG//:/_}"
            SAFE_NAME="${SAFE_NAME//\//_}"
            OUT="${TMP_DIR}/${SAFE_NAME}.tar.gz"
            docker save "${IMG}" | gzip > "${OUT}"
            ok "${IMG} saved ($(du -sh "${OUT}" | cut -f1))"
        done
    fi

    # -- Step 3: Push files to device ----------------------------------------------
    log "[3/6] Creating ${DEVICE_DIR} on device..."
    ${ADB} shell "mkdir -p ${DEVICE_DIR}"

    log "[3/6] Pushing image tarballs to device..."
    for F in "${TMP_DIR}"/*.tar.gz; do
        FNAME="$(basename "${F}")"
        log "  Pushing ${FNAME}..."
        ${ADB} push "${F}" "${DEVICE_DIR}/${FNAME}"
        ok "${FNAME} transferred"
    done

    log "[3/6] Pushing docker-compose.yml and openapi.json to device..."
    ${ADB} push docker-compose.yml "${DEVICE_DIR}/docker-compose.yml"
    ${ADB} push openapi.json "${DEVICE_DIR}/openapi.json"

    # Transfer SQL schema and migrations for automated DB initialization
    ${ADB} shell "mkdir -p ${DEVICE_DIR}/server/migrations"
    ${ADB} push server/migrations/001_initial_schema.sql                                           \
        "${DEVICE_DIR}/server/migrations/001_initial_schema.sql"
    ${ADB} push server/migrations/002_hnsw_index.sql                                               \
        "${DEVICE_DIR}/server/migrations/002_hnsw_index.sql"
    ${ADB} push server/migrations/003_metadata_gin_index.sql                                       \
        "${DEVICE_DIR}/server/migrations/003_metadata_gin_index.sql"
    ${ADB} push server/migrations/004_ingestion_jobs.sql                                           \
        "${DEVICE_DIR}/server/migrations/004_ingestion_jobs.sql"

    ok "Configuration assets transferred"

    # -- Step 4: Load images on device ---------------------------------------------
    log "[4/6] Loading images on device..."
    for F in "${TMP_DIR}"/*.tar.gz; do
        FNAME="$(basename "${F}")"
        log "  Loading ${FNAME}..."
        ${ADB} shell "docker load < ${DEVICE_DIR}/${FNAME}"
        ok "${FNAME} loaded"
    done

    # If not transferring public images, pull them on the device
    if [[ "${TRANSFER_PUBLIC_IMAGES}" != "1" ]]; then
        log "[4/6] Pulling public images on device (requires device internet)..."
        for IMG in "${PUBLIC_IMAGES[@]}"; do
            ${ADB} shell "docker pull --platform linux/arm64 ${IMG}"
            ok "${IMG} pulled on device"
        done
    fi

    # -- Step 5: Start the stack on device -----------------------------------------
    log "[5/6] Starting stack on device..."
    # Support both modern "docker compose" (preferred) and standalone "docker-compose"
    if ${ADB} shell "docker compose version 2>/dev/null" | grep -i -q "version"; then
        ${ADB} shell "cd ${DEVICE_DIR} && docker compose down && docker compose up -d"
    else
        ${ADB} shell "cd ${DEVICE_DIR} && docker-compose down && docker-compose up -d"
    fi
    ok "Stack started"

    # -- Step 6: Set up port forwarding for testing from PC ------------------------
    log "[6/6] Setting up ADB port forwarding..."
    ${ADB} forward tcp:${SERVER_PORT} tcp:${SERVER_PORT}     # Vector Store API
    ${ADB} forward tcp:${SWAGGER_PORT} tcp:${SWAGGER_PORT}   # Swagger UI
    ${ADB} forward tcp:${POSTGRES_PORT} tcp:${POSTGRES_PORT} # PostgreSQL (optional, for direct DB access)
    ok "Ports forwarded: ${SERVER_PORT} (API), ${SWAGGER_PORT} (Swagger UI), ${POSTGRES_PORT} (postgres)"

    # -- Done ----------------------------------------------------------------------
    echo ""
    echo "+==============================================================+"
    echo "   Deployment complete. Test from this PC:                      "
    echo "                                                                "
    echo "   Health:   curl http://localhost:${SERVER_PORT}/v1/health     "
    echo "   Swagger:  open http://localhost:${SWAGGER_PORT}              "
    echo "+==============================================================+"
}

# Invoke the function
echo "deploy_to_device"
echo "    Build Docker image and deploy it to device"
