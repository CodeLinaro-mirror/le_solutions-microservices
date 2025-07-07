# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from openapi_server.impl.constant import EnvVariableKeys

from openapi_server.main import app as application

TEST_LIBRARY_PATH = "/root/app/site-packages/test/libllmservice.so"

# Set the environment variable once for all tests
@pytest.fixture(scope="session", autouse=True)
def set_library_path():
    os.environ[EnvVariableKeys.ENV_LIBRARY_PATH_KEY] = TEST_LIBRARY_PATH

@pytest.fixture
def app() -> FastAPI:
    application.dependency_overrides = {}

    return application


@pytest.fixture
def client(app) -> TestClient:
    return TestClient(app)
