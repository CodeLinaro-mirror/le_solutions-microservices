# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import ctypes
from ctypes import c_void_p, c_uint32, c_bool, c_int, POINTER
from .graph_types import GraphInfo
from .lib_provider import QNN_INTERFACE_VER_TYPE, QnnLog_Callback_t

class QnnModelInterop:
    """Binds composeGraphs()/freeGraphsInfo from a model .so via ctypes."""
    def __init__(self, model_path: str):
        self.model_path = model_path
        self.lib = None
        self.compose = None
        self.free_graph_info = None

    def load(self):
        self.lib = ctypes.CDLL(self.model_path, mode=ctypes.RTLD_GLOBAL)

        # Compose
        for name in ("QnnModel_composeGraphs", "composeGraphs", "ComposeGraphs"):
            fn = getattr(self.lib, name, None)
            if fn:
                self.compose = fn
                break
        if not self.compose:
            raise RuntimeError("composeGraphs symbol not found (tried QnnModel_composeGraphs/composeGraphs/ComposeGraphs)")

        self.compose.restype = ctypes.c_int
        self.compose.argtypes = (
            c_void_p,                 # backend handle
            QNN_INTERFACE_VER_TYPE,   # QNN interface table (by value)
            c_void_p,                 # context handle
            POINTER(POINTER(c_void_p)),  # GraphConfigInfo_t** (NULL here)
            c_uint32,                 # num configs
            POINTER(POINTER(POINTER(GraphInfo))), # GraphInfo_t*** out
            POINTER(c_uint32),        # numGraphs out
            c_bool,                   # debug
            QnnLog_Callback_t,        # log callback
            c_int                     # log level
        )

        # Optional free
        for name in ("QnnModel_freeGraphsInfo", "freeGraphInfo", "FreeGraphInfo"):
            fn = getattr(self.lib, name, None)
            if fn:
                self.free_graph_info = fn
                self.free_graph_info.restype = ctypes.c_int
                self.free_graph_info.argtypes = (POINTER(POINTER(POINTER(GraphInfo))), c_uint32)
                break
