# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# QIMSDK Person Detect Image
FROM --platform=arm64 qimsdk:latest

# Enable color prompt
RUN sed -i 's/#force_color_prompt=yes/force_color_prompt=yes/g' /root/.bashrc

# Add script with mode 777 to /usr/bin
COPY --chmod=777 qimsdk_person_detect.sh /usr/bin
