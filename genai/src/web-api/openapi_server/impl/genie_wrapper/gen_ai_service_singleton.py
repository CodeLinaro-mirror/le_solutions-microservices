# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import sys
import pathlib
import threading
import time
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

    # Handle caching for ADHOC_MODE
    _handle_cache = {}  # model_id -> (handle, config_path, last_used_time)

    @classmethod
    def get_or_create_handle(cls, model_id, config_path, streaming=True):
        """
        Get a cached handle or create a new one.
        Enforces single active handle policy for ADHOC_MODE.
        """
        instance = cls()
        with cls._lock:
            # Check if we have a valid cached handle for this model
            if model_id in cls._handle_cache:
                handle, cached_config, _ = cls._handle_cache[model_id]
                if cached_config == config_path:
                    # Reset handle to clear KV cache (required for ADHOC context rebuilding)
                    instance.lib.llm_reset_object(handle)
                    cls._handle_cache[model_id] = (handle, config_path, time.time())
                    return handle

            # If we need to create a new handle, first destroy ALL existing handles
            # This enforces the "single active model" constraint
            if cls._handle_cache:
                for mid, (h, _, _) in list(cls._handle_cache.items()):
                    try:
                        instance.lib.llm_destroy_object(h)
                    except:
                        pass
                cls._handle_cache.clear()

            # Create new handle
            model_input = instance.ffi.new("char[]", model_id.encode('utf-8'))
            config_path_input = instance.ffi.new("char[]", config_path.encode('utf-8'))
            handle = instance.lib.llm_create_object(model_input, config_path_input, streaming)

            # Cache it
            if handle != instance.ffi.NULL:
                cls._handle_cache[model_id] = (handle, config_path, time.time())

            return handle

    @classmethod
    def destroy_cached_handle(cls, model_id):
        """Explicitly destroy a cached handle."""
        with cls._lock:
            if model_id in cls._handle_cache:
                handle, _, _ = cls._handle_cache[model_id]
                instance = cls()
                try:
                    instance.lib.llm_destroy_object(handle)
                except:
                    pass
                del cls._handle_cache[model_id]

    @classmethod
    def reset_singleton(cls):
        """
        Reset the singleton instance to force reinitialization.
        This releases the loaded library and allows fresh initialization.
        Used in ADHOC_MODE to ensure clean QAIRT resource management.
        Forces garbage collection to free DSP resources and prevent memory exhaustion.
        """
        with cls._lock:
            # Clear handle cache first
            if cls._handle_cache:
                 if cls._instance is not None and hasattr(cls._instance, 'lib') and cls._instance.lib:
                    for mid, (h, _, _) in list(cls._handle_cache.items()):
                        try:
                            cls._instance.lib.llm_destroy_object(h)
                        except:
                            pass
            cls._handle_cache.clear()

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
                if 'void llm_reset_object' not in updated_interface:
                    updated_interface += """
void llm_reset_object(LLMHandle handle);
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
