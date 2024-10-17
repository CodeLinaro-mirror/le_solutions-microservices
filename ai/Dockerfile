# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# QIMSDK Image with following microservices:
# 1. 24-stream ai-concurrency
# 2. access-control
# 3. daisy-chain
# 4. person-detect (restricted-zone)
# 5. ppe (personal protective equipment)

ARG QIMSDK_IMAGE=qimsdk:latest

FROM --platform=arm64 ${QIMSDK_IMAGE}

# Enable color prompt
RUN sed -i 's/#force_color_prompt=yes/force_color_prompt=yes/g' /home/qimsdk/.bashrc

# Add microservices scripts with mode 777 to /usr/bin
COPY --chmod=777 24-stream-ai-concurrency/qimsdk_24_stream_ai_concurrency.sh /usr/bin
COPY --chmod=777 access-control/qimsdk_access_control.sh /usr/bin
COPY --chmod=777 daisy-chain/qimsdk_daisy_chain.sh /usr/bin
COPY --chmod=777 person-detect/qimsdk_person_detect.sh /usr/bin
COPY --chmod=777 ppe/qimsdk_ppe.sh /usr/bin