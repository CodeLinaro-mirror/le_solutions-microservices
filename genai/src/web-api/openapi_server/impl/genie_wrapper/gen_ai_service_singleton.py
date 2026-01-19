# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import sys
import pathlib
import threading
from cffi import FFI
from openapi_server.impl.constant import EnvVariableValues, EnvVariableKeys

class LLMService:
    _instance = None
    _lock = threading.Lock()
    _initialization_error = None
    _circuit_breaker_open = False
    _failure_count = 0
    _max_failures = 3
    _circuit_breaker_timeout = 300  # 5 minutes
    _last_failure_time = None

    @classmethod
    def reset_singleton(cls):
        """
        Reset the singleton instance to force reinitialization.
        This releases the loaded library and allows fresh initialization.
        Used in ADHOC_MODE to ensure clean QAIRT resource management.
        Forces garbage collection to free DSP resources and prevent memory exhaustion.
        """
        with cls._lock:
            if cls._instance is not None:
                # CRITICAL: Properly release CFFI library resources
                # Without this, DSP memory accumulates and causes resource exhaustion
                if hasattr(cls._instance, 'lib') and cls._instance.lib:
                    try:
                        # Clear library and FFI references to release native resources
                        cls._instance.lib = None
                        cls._instance.ffi = None

                        # Force garbage collection to free DSP memory
                        import gc
                        gc.collect()

                        # Note: Successfully released LLM library resources
                    except Exception:
                        # Log but don't fail - library will be garbage collected eventually
                        pass

                # Clear the instance - this will trigger reinitialization on next access
                cls._instance = None

    @staticmethod
    def get_library_path():
        lib_path = os.getenv(EnvVariableKeys.ENV_LIBRARY_PATH_KEY)
        if not lib_path or not os.path.exists(lib_path):
            # Fallback to venv-relative path
            venv_path = pathlib.Path(sys.prefix)
            lib_path = venv_path / "lib" / "libllmservice.so"
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
                # Update the interface definition to include config_path parameter and error checking functions
                updated_interface = self.genai_interfaces.replace(
                    'LLMHandle llm_create_object(const char* model, bool streaming);',
                    'LLMHandle llm_create_object(const char* model, const char* config_path, bool streaming);'
                )
                # Add error checking functions to interface
                if 'bool llm_is_initialized' not in updated_interface:
                    updated_interface += """
bool llm_is_initialized(LLMHandle handle);
const char* llm_get_last_error(LLMHandle handle);
bool vlm_is_initialized(VLMHandle handle);
const char* vlm_get_last_error(VLMHandle handle);
"""
                self.ffi.cdef(updated_interface)
                self.lib = self.ffi.dlopen(LLMService.get_library_path())
                self.initialized = True

    @classmethod
    def is_circuit_breaker_open(cls):
        """Check if circuit breaker is open."""
        with cls._lock:
            if cls._circuit_breaker_open and cls._last_failure_time:
                import time
                # Check if timeout has passed
                if time.time() - cls._last_failure_time > cls._circuit_breaker_timeout:
                    # Reset circuit breaker
                    cls._circuit_breaker_open = False
                    cls._failure_count = 0
                    cls._last_failure_time = None
                    return False
            return cls._circuit_breaker_open

    @classmethod
    def get_initialization_error(cls):
        """Get the last initialization error."""
        return cls._initialization_error

    @classmethod
    def record_failure(cls, error_msg=None):
        """Record a failure and potentially open circuit breaker."""
        import time
        with cls._lock:
            cls._failure_count += 1
            cls._last_failure_time = time.time()
            if error_msg:
                cls._initialization_error = error_msg
            if cls._failure_count >= cls._max_failures:
                cls._circuit_breaker_open = True

    @classmethod
    def record_success(cls):
        """Record a success and reset failure count."""
        with cls._lock:
            cls._failure_count = 0
            cls._initialization_error = None
