# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
FROM ubuntu:22.04

# Install Python 3.10, create python group and user.
RUN apt-get update && \
    apt-get install -y python3-venv && \
    rm -rf /var/lib/apt/lists/*
RUN groupadd -g 1234 python && useradd -u 1234 -g python python

# Set up venv, set ownership to python user
RUN python3 -m venv /venv
ENV PATH=/venv/bin:$PATH
RUN chown -hR python /venv


WORKDIR /app

COPY --chown=python:python requirements.txt .
RUN pip install -r requirements.txt

COPY --chown=python:python ./app .

USER python
CMD ["python", "people_analytics_server.py"]
