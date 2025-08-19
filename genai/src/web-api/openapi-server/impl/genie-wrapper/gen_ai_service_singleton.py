# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import threading
from cffi import FFI
from openapi_server.impl.constant import EnvVariableValues, EnvVariableKeys, GENAI_INTERFACE_FILE

class LLMService:
    _instance = None
    _lock = threading.Lock()

    @staticmethod
    def get_library_path():
        return os.getenv(EnvVariableKeys.ENV_LIBRARY_PATH_KEY, EnvVariableValues.ENV_LIBRARY_PATH_DEFAULT_VAL)

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance.__init__()
        return cls._instance

    def __init__(self):
    	if not hasattr(self, 'initialized'):
                with open(GENAI_INTERFACE_FILE, 'r') as f:
                      self.genai_interfaces = f.read()
                self.ffi = FFI()
                self.ffi.cdef(self.genai_interfaces)
                self.lib = self.ffi.dlopen(LLMService.get_library_path())
                self.initialized = True
