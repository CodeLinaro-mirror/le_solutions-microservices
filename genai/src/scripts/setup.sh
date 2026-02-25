#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

function print-red() {
    tput setaf 1 2>/dev/null
    echo $@
    tput sgr0 2>/dev/null
    true
}

function print-green() {
    tput setaf 2 2>/dev/null
    echo $@
    tput sgr0 2>/dev/null
    true
}

function print-yellow() {
    tput setaf 3 2>/dev/null
    echo $@
    tput sgr0 2>/dev/null
    true
}

function print-blue() {
    tput setaf 4 2>/dev/null
    echo $@
    tput sgr0 2>/dev/null
    true
}

[ -n "${LLM_SCRIPTS_DIR}" ] && [ -d "${LLM_SCRIPTS_DIR}" ]                                      && {
    SCRIPTS="${LLM_SCRIPTS_DIR}"
}

[ -n "${CHATCOMPLETIONS_SCRIPTS_DIR}" ] && [ -d "${CHATCOMPLETIONS_SCRIPTS_DIR}" ]              && {
    SCRIPTS="${CHATCOMPLETIONS_SCRIPTS_DIR}"
}

[ -z "${SCRIPTS}" ]                                                                             && {
    print-red "Error: No scripts directory found"
    return -1  #
}

# Setup helper scripts
for script in ${SCRIPTS}/*.sh; do
    # Skip current file
    [ ${script} == "${SCRIPTS}/setup.sh" ]                                                      && {
        continue
    }

    source ${script}
done
