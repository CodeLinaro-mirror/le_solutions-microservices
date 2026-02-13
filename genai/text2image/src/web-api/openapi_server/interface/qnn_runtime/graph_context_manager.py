# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import ctypes
from ctypes import c_void_p, c_uint32
from typing import Optional, Tuple

from .lib_provider import QnnProvider, SystemProvider
from .qnn_types import QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH
from .graph_types import GraphInfo
from .model_interop import QnnModelInterop
from .system_structs import QnnSystemContext_BinaryInfo_t
from .utils_dump import print


class GraphContextManager:
    """
    Encapsulates:
      - Context creation (from binary or fresh)
      - Graph composition OR loading-from-context-binary
      - Graph finalization and BinaryInfo traversal
      - Lifetime of temporary arrays/struct pointers to prevent GC
      - Lifetime of the system context used when parsing context binaries
    """

    def __init__(
        self,
        *,
        opts,
        qnn_provider: QnnProvider,
        system_provider: Optional[SystemProvider],
        backend: c_void_p,
        device: c_void_p,
        profile: c_void_p,
    ) -> None:
        self.opts = opts
        self.qnn_provider = qnn_provider
        self.system_provider = system_provider
        self.backend = backend
        self.device = device
        self.profile = profile

        # Outputs/state
        self.context = c_void_p()
        self._graphs_ppp = None
        self._graphs_count = 0

        # Internals retained to keep C-level memory alive
        self._model_interop: Optional[QnnModelInterop] = None
        self._sys_ctx = None
        self._gi_instances = None
        self._gi_ptrs = None
        self._gi_pp_array = None
        self._gi_pp_ptr = None

    # --------------------------
    # Public API
    # --------------------------
    def create_context(self) -> c_void_p:
        """
        Re-homed from QnnSampleApp._create_context()
        Creates context either from a saved binary or fresh.
        """
        if self.opts.retrieve_context:
            with open(self.opts.retrieve_context, 'rb') as f:
                blob = f.read()
            if not blob:
                raise RuntimeError(f"Empty context binary: {self.opts.retrieve_context}")

            rc = self.qnn_provider.contextCreateFromBinary(
                self.backend,
                self.device,
                ctypes.POINTER(ctypes.POINTER(c_void_p))(),  # configs = NULL
                ctypes.cast(blob, c_void_p),
                ctypes.c_size_t(len(blob)),
                ctypes.byref(self.context),
                self.profile,
            )
            print(f"[CALL] contextCreateFromBinary rc={rc} "
                  f"context=0x{(self.context.value or 0):016x}")
            if rc != 0:
                raise RuntimeError(f"contextCreateFromBinary failed rc={rc}")
        else:
            rc = self.qnn_provider.contextCreate(
                self.backend,
                self.device,
                ctypes.POINTER(ctypes.POINTER(c_void_p))(),  # configs = NULL
                ctypes.byref(self.context)
            )
            print(f"[CALL] contextCreate rc={rc} context=0x{(self.context.value or 0):016x}")
            if rc != 0:
                raise RuntimeError(f"contextCreate failed rc={rc}")

        return self.context

    def compose_and_finalize_graphs(self) -> Tuple[ctypes.POINTER(ctypes.POINTER(ctypes.POINTER(GraphInfo))), int]:
        """
        Re-homed from QnnSampleApp._compose_and_finalize_graphs()
        - If model .so is provided, compose graphs and finalize
        - Else if retrieve_context is provided, load graphs from binary and finalize
        """
        self._ensure_system_interface_is_sane()

        if self.opts.model_path:
            if self._model_interop is None:
                self._model_interop = QnnModelInterop(self.opts.model_path)
            self._model_interop.load()

            graphs_ppp, graphs_count = self.qnn_provider.compose_graphs(
                backend_handle=self.backend,
                context_handle=self.context,
                model_interop=self._model_interop,
                debug=self.opts.debug,
                log_level=self.opts.log_level,
            )

            self._graphs_ppp = graphs_ppp
            self._graphs_count = graphs_count
            print(f"[INFO] composeGraphs produced {self._graphs_count} graph(s).")

            self._finalize_graph_list()
            return self._graphs_ppp, self._graphs_count

        if self.opts.retrieve_context:
            print("[INFO] No model .so provided; loading graphs from context binary via System API")
            self._load_graphs_from_context_binary()
            print(f"[INFO] Loaded {self._graphs_count} graph(s) from context binary.")
            return self._graphs_ppp, self._graphs_count

        print("[FATAL] Neither --model_path nor --retrieve_context was provided; cannot build graphs.")
        raise RuntimeError(
            "No model_path (.so) or retrieve_context (.bin) specified. "
            "Provide one of them in AppOptions to build graphs."
        )

    def get_graphs_ptr_and_count(self):
        return self._graphs_ppp, self._graphs_count

    def cleanup(self) -> None:
        """Free system context created during context-binary inspection."""
        try:
            if getattr(self, "_sys_ctx", None):
                rc = self.system_provider.systemContextFree(self._sys_ctx)
                print(f"[CALL] systemContextFree rc={rc}")
        except Exception:
            pass
        self._sys_ctx = None

    # --------------------------
    # Internal helpers
    # --------------------------
    def _check_finalize_capability(self) -> int:
        support_finalize_deserialized_graph = int(QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH)
        return int(self.qnn_provider.propertyHasCapability(support_finalize_deserialized_graph))

    def _finalize_graph_list(self) -> None:
        # 1) Capability gate
        rc = self._check_finalize_capability()
        if rc != 0:
            print("[INFO] Backend does not support finalizing deserialized graphs")
            return

        # 2) Finalize loop
        base_pp = self._graphs_ppp.contents
        arr = ctypes.cast(base_pp, ctypes.POINTER(ctypes.POINTER(GraphInfo)))
        for i in range(self._graphs_count):
            gi = arr[i].contents
            rc_f = self.qnn_provider.graphFinalize(gi.graph, self.profile, c_void_p())
            name = gi.graphName.decode() if gi.graphName else ''
            print(f'[CALL] graphFinalize[{i}] rc={rc_f} name={name}')
            if rc_f != 0:
                raise RuntimeError(f'graphFinalize[{i}] failed rc={rc_f}')

    def _load_graphs_from_context_binary(self) -> None:
        if not self.opts.retrieve_context:
            raise RuntimeError("retrieve_context not set")
        if self.system_provider is None:
            raise RuntimeError("System provider not loaded; set system_library in AppOptions")

        # ---- read blob ----
        with open(self.opts.retrieve_context, "rb") as f:
            blob = f.read()
        if not blob:
            raise RuntimeError(f"Empty context binary: {self.opts.retrieve_context}")

        # ---- create system context ----
        sys_ctx = c_void_p()
        rc = self.system_provider.systemContextCreate(ctypes.byref(sys_ctx))
        if rc != 0 or not sys_ctx:
            raise RuntimeError(f"systemContextCreate failed rc={rc}")
        self._sys_ctx = sys_ctx

        # ---- getBinaryInfo ----
        bi_pp = ctypes.POINTER(QnnSystemContext_BinaryInfo_t)()
        bi_size = ctypes.c_size_t(0)
        rc_bi = self.system_provider.systemContextGetBinaryInfo(
            self._sys_ctx, ctypes.cast(blob, c_void_p), ctypes.c_uint64(len(blob)), ctypes.byref(bi_pp), ctypes.byref(bi_size)
        )
        have_bi = (rc_bi == 0) and bool(bi_pp)
        if not have_bi:
            try:
                self.system_provider.systemContextFree(self._sys_ctx)
            except Exception:
                pass
            raise RuntimeError(f"no binary info (rc={rc_bi}) available")

        BI = bi_pp.contents
        v = int(BI.version)
        print(f"[DEBUG] BinaryInfo.version={v}")

        # ---- select graphs pointer and count ----
        if v == 1:
            graphs_ptr = BI.u.contextBinaryInfoV1.graphs
            num_graphs = int(BI.u.contextBinaryInfoV1.numGraphs)
        elif v == 2:
            graphs_ptr = BI.u.contextBinaryInfoV2.graphs
            num_graphs = int(BI.u.contextBinaryInfoV2.numGraphs)
        elif v == 3:
            graphs_ptr = BI.u.contextBinaryInfoV3.graphs
            num_graphs = int(BI.u.contextBinaryInfoV3.numGraphs)
        else:
            raise RuntimeError(f"Unsupported BinaryInfo version: {v}")

        if num_graphs <= 0 or not bool(graphs_ptr):
            try:
                self.system_provider.systemContextFree(self._sys_ctx)
            except Exception:
                pass
            raise RuntimeError("No graphs found in BinaryInfo; check SDK/layout compatibility.")

        print(f"[INFO] Using BinaryInfo: num_graphs={num_graphs}")

        # ---- collect and retrieve graph handles ----
        graphinfo_instances, graph_ptrs = [], []
        for i in range(num_graphs):
            gi_src = graphs_ptr[i]
            vgi = int(gi_src.version)
            if vgi == 1:
                gi_in = gi_src.u.graphInfoV1
            elif vgi == 2:
                gi_in = gi_src.u.graphInfoV2
            elif vgi == 3:
                gi_in = gi_src.u.graphInfoV3
            else:
                raise RuntimeError(f"Graph[{i}] unsupported info version: {vgi}")

            name = gi_in.graphName or b""
            graph_handle = c_void_p()
            rc = self.qnn_provider.graphRetrieve(self.context, name, ctypes.byref(graph_handle))
            if rc != 0 or not graph_handle:
                try:
                    gname = name.decode() if name else ""
                except Exception:
                    gname = "<decode error>"
                raise RuntimeError(f"graphRetrieve failed rc={rc} name={gname}")

            gi_local = GraphInfo()
            gi_local.graph = graph_handle
            gi_local.graphName = name
            gi_local.inputTensors = gi_in.graphInputs
            gi_local.numInputTensors = gi_in.numGraphInputs
            gi_local.outputTensors = gi_in.graphOutputs
            gi_local.numOutputTensors = gi_in.numGraphOutputs
            graphinfo_instances.append(gi_local)
            graph_ptrs.append(ctypes.pointer(gi_local))

            try:
                gname = name.decode() if name else ""
            except Exception:
                gname = "<decode error>"
            print(f"[INFO] Graph[{i}] name='{gname}' "
                  f"inputs={int(gi_in.numGraphInputs)} outputs={int(gi_in.numGraphOutputs)}")

        # ---- build GraphInfo*** and finalize ----
        PP_GraphInfo = ctypes.POINTER(GraphInfo)
        pp_array = (PP_GraphInfo * num_graphs)()
        for i in range(num_graphs):
            pp_array[i] = graph_ptrs[i]
        pp_ptr = ctypes.cast(pp_array, ctypes.POINTER(PP_GraphInfo))

        self._graphs_ppp = ctypes.pointer(pp_ptr)
        self._graphs_count = num_graphs
        self._gi_instances = graphinfo_instances
        self._gi_ptrs = graph_ptrs
        self._gi_pp_array = pp_array
        self._gi_pp_ptr = pp_ptr

        self._finalize_graph_list()

    def _ensure_system_interface_is_sane(self):
        # Check that systemProvider exists and function pointers look plausible.
        if self.system_provider is None:
            raise RuntimeError("System provider not loaded; set system_library in AppOptions")

        iface = self.system_provider._SystemProvider__interface()
        ptrs = [
            iface.systemContextCreate,
            iface.systemContextGetBinaryInfo,
            getattr(iface, "systemContextGetMetaData", 0),
            iface.systemContextFree,
        ]
        # Basic sanity: all pointers should be non-zero and “reasonable”
        for name, p in zip(
            ["systemContextCreate", "systemContextGetBinaryInfo",
             "systemContextGetMetaData", "systemContextFree"],
            ptrs
        ):
            addr = int(p) if not isinstance(p, int) else p
            if addr < 0x1000:
                raise RuntimeError(
                    f"System interface pointer for {name} looks invalid: 0x{addr:016x}. "
                    "This usually indicates a QNN_SYSTEM_INTERFACE_VER_TYPE layout mismatch.\n"
                    "Please ensure lib_provider.py's QNN_SYSTEM_INTERFACE_VER_TYPE "
                    "fields match SDK exactly."
                )
