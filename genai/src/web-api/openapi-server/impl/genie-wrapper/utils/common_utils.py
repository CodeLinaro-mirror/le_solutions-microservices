# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from openapi_server.impl.constant import LLMServiceQueryConstant as QUERY_CONST

class CommonUtils:
    @staticmethod
    def copy_py_string_to_c_array(ffi, c_array, py_string: str, max_length: int):
        """
        Safely copies a Python string into a fixed-size C char array using CFFI.

		Parameters:
		- ffi: The CFFI FFI instance.
		- c_array: The C char array field (e.g., struct.field).
		- py_string: The Python string to copy.
		- max_length: The total size of the C char array (including space for null terminator).
		"""
        encoded = py_string.encode('utf-8')[:max_length - 1]  # Reserve space for null terminator
        ffi.memmove(c_array, encoded, len(encoded))
        c_array[len(encoded)] = b'\0'  # Ensure null termination

    @staticmethod
    def get_model_name():
        return os.getenv("GENAI_MODEL_NAME", QUERY_CONST.DEFAULT_MODEL)
