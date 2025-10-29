#!/bin/bash

# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Push artifacts to device
function VLM-push-artifacts() {
    (
        cd ${VLM_DEPLOY_DIR}                                                                    || {
            print-red "No such dir: ${VLM_DEPLOY_DIR}"
            return -1
        }

        tar cf VLM_dev_artifacts.tar ./*                                                        || {
            print-red "tar cf VLM_dev_artifacts.tar failed !!!"
            return -1
        }

        # Please note that:
        # container name "genai_chatcompletion_service" is hardcoded
        # because it is also hardcoded in docker-compose files
        adb shell "mkdir -p /tmp/qti/development"                                               && \
            adb push VLM_dev_artifacts.tar /tmp/qti/development/                                && \
            adb shell "cd /tmp/qti/development                                                  && \
                tar -xf /tmp/qti/development/VLM_dev_artifacts.tar                              && \
                docker cp usr genai_chatcompletion_service:/"                                   && \
            adb shell "rm -rf /tmp/qti/development/usr"                                         || {

            print-red "Artifacts push failed !!!"

            adb shell "rm -rf /tmp/qti/development/usr"
            adb shell "rm -f /tmp/qti/development/VLM_dev_artifacts.tar"

            rm -f VLM_dev_artifacts.tar

            return -1
        }

        adb shell "rm -f /tmp/qti/development/VLM_dev_artifacts.tar"
        rm -f VLM_dev_artifacts.tar
    )

    print-green "Dev artifacts pushed to device !!!"
    return 0
}

# Push venv to device
function VLM-push-venv() {
    (
        cd /mnt/work/                                                                           || {
            print-red "No such dir: /mnt/work/"
            return -1
        }

        tar cf VLM_dev_venv.tar venv                                                            || {
            print-red "tar cf VLM_dev_venv.tar failed !!!"
            return -1
        }

        # Please note that:
        # container name "genai_chatcompletion_service" is hardcoded
        # because it is also hardcoded in docker-compose files
        adb shell "mkdir -p /tmp/qti/development"                                               && \
            adb push VLM_dev_venv.tar /tmp/qti/development/                                     && \
            adb shell "cd /tmp/qti/development                                                  && \
                tar -xf /tmp/qti/development/VLM_dev_venv.tar                                   && \
                docker cp venv genai_chatcompletion_service:/mnt/work/"                         && \
            adb shell "rm -rf /tmp/qti/development/venv"                                        || {

            print-red "Artifacts push failed !!!"

            adb shell "rm -rf /tmp/qti/development/venv"
            adb shell "rm -f /tmp/qti/development/VLM_dev_venv.tar"

            rm -f VLM_dev_venv.tar

            return -1
        }

        adb shell "rm -f /tmp/qti/development/VLM_dev_venv.tar"
        rm -f VLM_dev_venv.tar
    )

    print-green "Dev artifacts pushed to device !!!"
    return 0
}

print-green "VLM-push-artifacts"
echo "    Push artifacts to device"
print-green "VLM-push-venv"
echo "    Push venv to device"
