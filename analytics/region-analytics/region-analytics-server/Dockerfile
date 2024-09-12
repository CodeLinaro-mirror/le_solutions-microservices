# Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear
FROM docker-registry.qualcomm.com/library/python:3.11-alpine

COPY requirements.txt .
RUN pip install -r requirements.txt

COPY . .

CMD ["python", "region_analytics_server.py"]
