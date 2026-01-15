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

    @classmethod
    def reset_singleton(cls):
        """
        Reset the singleton instance to force reinitialization.
        This releases the loaded library and allows fresh initialization.
        Used in ADHOC_MODE to ensure clean QAIRT resource management.
        """
        with cls._lock:
            if cls._instance is not None:
                # Try to close the library (CFFI may not support dlclose on all platforms)
                if hasattr(cls._instance, 'lib') and hasattr(cls._instance, 'ffi'):
                    try:
                        # Note: dlclose is not always available in CFFI
                        # This is a best-effort cleanup
                        if hasattr(cls._instance.ffi, 'dlclose'):
                            cls._instance.ffi.dlclose(cls._instance.lib)
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
                # Update the interface definition to include config_path parameter
                updated_interface = self.genai_interfaces.replace(
                    'LLMHandle llm_create_object(const char* model, bool streaming);',
                    'LLMHandle llm_create_object(const char* model, const char* config_path, bool streaming);'
                )
                self.ffi.cdef(updated_interface)
                self.lib = self.ffi.dlopen(LLMService.get_library_path())
                self.initialized = True
