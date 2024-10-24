#!/bin/bash
# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
if [ $(date +%Y) -lt "2024" ]; then
    echo "Date has not been set properly: == `date` =="
    exit 1
fi

echo "Current Date --> `date`"

