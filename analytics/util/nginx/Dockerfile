# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
FROM docker-registry.qualcomm.com/library/nginx:alpine

COPY ./default.conf /etc/nginx/conf.d/default.conf

COPY ./index.html /usr/share/nginx/html/index.html

EXPOSE 80
# CMD ["echo $server_name"]
