# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import sys
import pathlib
import threading
import time
from cffi import FFI
from openapi_server.impl.constant import EnvVariableValues, EnvVariableKeys

class VLMService:
    _instance = None
    _lock = threading.Lock()

    # Circuit breaker pattern for handling repeated failures
    _circuit_breaker_open = False
    _failure_count = 0
    _max_failures = 3
    _circuit_breaker_timeout = 300  # 5 minutes
    _circuit_breaker_open_time = None
    _last_error_message = None

    @staticmethod
    def get_library_path():
        lib_path = os.getenv(EnvVariableKeys.ENV_VLM_LIBRARY_PATH_KEY)
        if not lib_path or not os.path.exists(lib_path):
            # Fallback to venv-relative path
            venv_path = pathlib.Path(sys.prefix)
            lib_path = venv_path / "lib" / "libvlmservice.so"
        return str(lib_path)

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance.__init__()
        return cls._instance

    def __init__(self):
        if not hasattr(self, 'initialized'):
            header_path = os.getenv(EnvVariableKeys.GENAI_INTERFACE_FILE_KEY)
            if not header_path or not os.path.exists(header_path):
                # Fallback to a path relative to the virtual environment
                venv_path = pathlib.Path(sys.prefix)
                header_path = venv_path / "include" / "genai_interface.h"

            with open(header_path, 'r') as f:
                self.genai_interfaces = f.read()

            self.ffi = FFI()
            # Parse the interface definition for VLM functions
            self.ffi.cdef(self.genai_interfaces)
            self.lib = self.ffi.dlopen(VLMService.get_library_path())
            self.initialized = True

    @classmethod
    def is_circuit_breaker_open(cls):
        """Check if circuit breaker is open (service unavailable due to repeated failures)"""
        if not cls._circuit_breaker_open:
            return False

        # Check if timeout has elapsed
        if cls._circuit_breaker_open_time:
            elapsed = time.time() - cls._circuit_breaker_open_time
            if elapsed >= cls._circuit_breaker_timeout:
                # Reset circuit breaker
                cls._circuit_breaker_open = False
                cls._failure_count = 0
                cls._circuit_breaker_open_time = None
                cls._last_error_message = None
                return False

        return True

    @classmethod
    def record_failure(cls, error_message=None):
        """Record a failure and potentially open the circuit breaker"""
        cls._failure_count += 1
        cls._last_error_message = error_message

        if cls._failure_count >= cls._max_failures:
            cls._circuit_breaker_open = True
            cls._circuit_breaker_open_time = time.time()

    @classmethod
    def record_success(cls):
        """Record a successful operation and reset failure count"""
        cls._failure_count = 0
        cls._last_error_message = None

    @classmethod
    def get_initialization_error(cls):
        """Get the last initialization error message"""
        if cls._circuit_breaker_open:
            elapsed = time.time() - cls._circuit_breaker_open_time if cls._circuit_breaker_open_time else 0
            remaining = max(0, cls._circuit_breaker_timeout - elapsed)
            return f"VLM service circuit breaker open. Retry in {int(remaining)} seconds. Last error: {cls._last_error_message}"
        return cls._last_error_message
