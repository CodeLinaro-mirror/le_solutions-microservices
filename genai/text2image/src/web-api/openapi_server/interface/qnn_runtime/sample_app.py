# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import ctypes
import re
from copy import deepcopy
from ctypes import c_void_p, c_uint32
from typing import Optional, List

from .options import AppOptions
from .lib_provider import QnnLibrary, QnnProvider, SystemProvider, CORE_LOG_CB, ErrorHelper
from .qnn_types import QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH, Qnn_Tensor_t
from .graph_types import GraphInfo
from .model_interop import QnnModelInterop
from .io_tensor import IOTensor, tv_view
from .system_structs import QnnSystemContext_BinaryInfo_t
from .utils_dump import print
from .graph_context_manager import GraphContextManager


class QnnSampleApp:
    def __init__(self, opts: AppOptions, autoload_libs: bool = True) -> None:
        self.opts = opts
        os.makedirs(self.opts.output_dir, exist_ok=True)

        backend_name = os.path.basename(
            opts.backend_path) if getattr(opts, "backend_path", None) else "libQnnHtp.so"
        system_name = os.path.basename(
            opts.system_library) if getattr(opts, "system_library", None) else "libQnnSystem.so"

        self.libs = QnnLibrary(backend_name=backend_name,
                               system_name=system_name)

        self.qnn_provider: Optional[QnnProvider] = None
        self.system_provider: Optional[SystemProvider] = None

        # Handles
        self.backend = c_void_p()
        self.logger = c_void_p()
        self.device = c_void_p()
        self.context = c_void_p()
        self.profile = c_void_p()

        # Legacy single-manager fields (kept for compatibility)
        self._graphs_ppp = None
        self._graphs_count = 0

        # Multi-manager support
        self._graph_mgrs: List[GraphContextManager] = []

        if autoload_libs:
            self._load_libs()

    # --- Context manager ---
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.stop()
        return False

    def __del__(self):
        try:
            self.stop()
        except Exception:
            pass

    # --- internal helpers ---
    def _load_libs(self):
        self.libs.load()
        pp, n = self.libs.get_qnn_providers()
        self.qnn_provider = QnnProvider(pp[0])
        info = self.qnn_provider.info()
        print(f"[QNN] provider=0x{info['addr']:016x} "
              f"name={info['providerName']} "
              f"coreApi={info['coreApi']} "
              f"backendApi={info['backendApi']}")

        if self.libs.system is not None:
            try:
                spp, sn = self.libs.get_system_providers()
                if sn > 0:
                    self.system_provider = SystemProvider(spp[0])
                    sinfo = self.system_provider.info()
                    print(f"[SYS] provider=0x{sinfo['addr']:016x} "
                          f"name={sinfo['providerName']} "
                          f"systemApi={sinfo['systemApi']}")
            except RuntimeError as e:
                print("[WARN]", e)

    def _init_logging(self):
        rc = self.qnn_provider.logCreate(CORE_LOG_CB, int(
            self.opts.log_level), ctypes.byref(self.logger))
        print(f"[CALL] logCreate rc={rc} "
              f"logger=0x{(ctypes.cast(self.logger, c_void_p).value or 0):016x}")
        if rc != 0:
            self.logger = c_void_p()

    def _init_backend(self):
        configs = ctypes.POINTER(ctypes.POINTER(c_void_p))()
        rc = self.qnn_provider.backendCreate(
            self.logger, configs, ctypes.byref(self.backend))
        print(f"[CALL] backendCreate rc={rc} "
              f"backend=0x{(self.backend.value or 0):016x}")
        if rc != 0:
            raise RuntimeError(f"backendCreate failed rc={rc}")

    def _register_op_packages(self):
        if not getattr(self.opts, "op_packages", None):
            return
        for item in self.opts.op_packages.split(','):
            parts = item.split(':')
            if len(parts) not in (2, 3):
                raise ValueError(f"Malformed op package spec: {item}")
            path, provider = parts[0], parts[1]
            target = parts[2] if len(parts) == 3 else None
            rc = self.qnn_provider.backendRegisterOpPackage(
                self.backend, path.encode(), provider.encode(), (target or '').encode()
            )
            print(f"[CALL] backendRegisterOpPackage rc={rc} "
                  f"path={path} provider={provider} target={target}")
            if rc != 0:
                raise RuntimeError(f"backendRegisterOpPackage failed rc={rc}")

    def _create_device(self):
        rc = self.qnn_provider.deviceCreate(self.logger, ctypes.POINTER(
            ctypes.POINTER(c_void_p))(), ctypes.byref(self.device))
        print(f"[CALL] deviceCreate rc={rc} "
              f"device=0x{(self.device.value or 0):016x}")
        if rc != 0:
            raise RuntimeError(f"deviceCreate failed rc={rc}")

    def _init_profiling(self):
        if getattr(self.opts, "profiling_level", "off") == "off":
            return
        lvl_map = {"basic": 1, "detailed": 2}
        lvl = lvl_map.get(self.opts.profiling_level, 1)
        rc = self.qnn_provider.profileCreate(
            self.backend, int(lvl), ctypes.byref(self.profile))
        print(f"[CALL] profileCreate rc={rc} profile=0x{(self.profile.value or 0):016x}")
        if rc != 0:
            raise RuntimeError(f"profileCreate failed rc={rc}")

    def _resolve_retrieve_context_paths(self) -> List[str]:
        """
        Returns a list of context-binary paths to load.

        Priority:
          1) opts.retrieve_contexts (list or comma/semicolon-separated string)
          2) opts.retrieve_context:
             - if directory: look for the 3 Stable Diffusion component bins
             - if string with commas/semicolons: split
             - else: single path
        """
        candidates = [
            "stable_diffusion_v2_1-vae-qualcomm_sa8775p.bin",
            "stable_diffusion_v2_1-text_encoder-qualcomm_sa8775p.bin",
            "stable_diffusion_v2_1-unet-qualcomm_sa8775p.bin",
        ]

        paths: List[str] = []

        # 1) Explicit list param (if AppOptions exposes it)
        rc_list = getattr(self.opts, "retrieve_contexts", None)
        if rc_list:
            if isinstance(rc_list, (list, tuple)):
                paths.extend([str(p) for p in rc_list])
            elif isinstance(rc_list, str):
                parts = [p.strip() for p in rc_list.replace(";", ",").split(",") if p.strip()]
                paths.extend(parts)

        # 2) Fallback to retrieve_context
        if not paths:
            rc = getattr(self.opts, "retrieve_context", None)
            if rc:
                if os.path.isdir(rc):
                    for name in candidates:
                        p = os.path.join(rc, name)
                        if os.path.exists(p):
                            paths.append(p)
                else:
                    if any(sep in rc for sep in [",", ";"]):
                        parts = [p.strip() for p in rc.replace(";", ",").split(",") if p.strip()]
                        paths.extend(parts)
                    else:
                        paths.append(rc)

        # Deduplicate while preserving order
        dedup, seen = [], set()
        for p in paths:
            if p not in seen:
                dedup.append(p)
                seen.add(p)

        return dedup

    # --- lifecycle ---
    def start(self) -> None:
        if self.qnn_provider is None:
            self._load_libs()
        self._init_logging()
        self._init_backend()
        self._register_op_packages()
        self._create_device()
        self._init_profiling()

        multi_paths = self._resolve_retrieve_context_paths()

        if len(multi_paths) > 1:
            print(f"[INFO] Detected {len(multi_paths)} context binaries; creating one GraphContextManager per file.")
            self._graph_mgrs = []
            for p in multi_paths:
                o = deepcopy(self.opts)
                if hasattr(o, "model_path"):
                    o.model_path = None
                o.retrieve_context = p
                mgr = GraphContextManager(
                    opts=o,
                    qnn_provider=self.qnn_provider,
                    system_provider=self.system_provider,
                    backend=self.backend,
                    device=self.device,
                    profile=self.profile
                )
                _ = mgr.create_context()
                _ = mgr.compose_and_finalize_graphs()
                self._graph_mgrs.append(mgr)

            # Clear legacy single-manager fields
            self.context = c_void_p()
            self._graphs_ppp = None
            self._graphs_count = 0

        else:
            # Single-manager (legacy behavior)
            self._graph_mgrs = []
            mgr = GraphContextManager(
                opts=self.opts,
                qnn_provider=self.qnn_provider,
                system_provider=self.system_provider,
                backend=self.backend,
                device=self.device,
                profile=self.profile
            )
            self.context = mgr.create_context()
            self._graphs_ppp, self._graphs_count = mgr.compose_and_finalize_graphs()
            self._graph_mgrs.append(mgr)

    def stop(self) -> None:
        # Free system contexts held by managers
        try:
            for mgr in self._graph_mgrs:
                mgr.cleanup()
        except Exception:
            pass

        # Free each manager's QNN context
        try:
            for mgr in self._graph_mgrs:
                if mgr.context and mgr.context.value:
                    rc = self.qnn_provider.contextFree(mgr.context, self.profile)
                    print(f"[CALL] contextFree rc={rc}")
        except Exception:
            pass

        self.context = c_void_p()

        try:
            if self.device and self.device.value:
                rc = self.qnn_provider.deviceFree(self.device)
                print(f"[CALL] deviceFree rc={rc}")
        except Exception:
            pass
        self.device = c_void_p()

        try:
            if self.backend and self.backend.value:
                rc = self.qnn_provider.backendFree(self.backend)
                print(f"[CALL] backendFree rc={rc}")
        except Exception:
            pass
        self.backend = c_void_p()

        try:
            if self.logger and self.logger.value:
                rc = self.qnn_provider.logFree(self.logger)
                print(f"[CALL] logFree rc={rc}")
        except Exception:
            pass
        self.logger = c_void_p()

        self._graph_mgrs = []

    # --- execution helpers ---
    def _execute_graphs_for_manager(self, mgr: GraphContextManager) -> int:
        """
        Executes all graphs owned by a given manager. Mirrors the legacy execute_graphs() loop.
        """
        import ctypes

        if not mgr._graphs_ppp or not bool(mgr._graphs_ppp):
            raise RuntimeError(
                "No graphs loaded (graphs_ppp is null). "
                "Ensure compose/context loader ran."
            )

        base_pp = mgr._graphs_ppp.contents
        arr = ctypes.cast(base_pp, ctypes.POINTER(ctypes.POINTER(GraphInfo)))

        io = IOTensor()
        for i in range(mgr._graphs_count):
            gi = arr[i].contents
            graph_name = gi.graphName.decode() if gi.graphName else ""

            inputs, outputs, in_buf_refs, out_buf_refs = io.setup_input_output(
                gi, system_provider=self.system_provider
            )

            # Guard: input population
            try:
                if getattr(self.opts, "input_list_paths", None):
                    io.populate_inputs(
                        inputs, gi, self.opts.input_list_paths,
                        input_data_type=self.opts.input_data_type
                    )
            except Exception as e:
                print("[ERR] populate_inputs failed:", e)
                raise

            def _assert_non_null_buffers(gi, inputs, outputs):
                for ii in range(gi.numInputTensors):
                    _, tv = tv_view(inputs[ii])
                    ptr = ctypes.cast(tv.clientBuf.data, c_void_p).value or 0
                    if ptr == 0:
                        name = tv.name.decode() if tv.name else f"Input_{ii}"
                        raise RuntimeError(f"NULL input buffer at {ii} ({name})")
                    tv.memType = c_uint32(0)

                for oi in range(gi.numOutputTensors):
                    _, tv = tv_view(outputs[oi])
                    ptr = ctypes.cast(tv.clientBuf.data, c_void_p).value or 0
                    if ptr == 0:
                        name = tv.name.decode() if tv.name else f"Output_{oi}"
                        raise RuntimeError(f"NULL output buffer at {oi} ({name})")
                    tv.memType = c_uint32(0)

            _assert_non_null_buffers(gi, inputs, outputs)

            def _as_tensor_array(objs, n):
                if hasattr(objs, "_length_") and isinstance(objs, ctypes.Array):
                    return objs
                if isinstance(objs, list):
                    ArrT = Qnn_Tensor_t * int(n)
                    return ArrT(*objs)
                ArrT = Qnn_Tensor_t * int(n)
                return ArrT(*[objs])

            inputs_arr = _as_tensor_array(inputs, gi.numInputTensors)
            outputs_arr = _as_tensor_array(outputs, gi.numOutputTensors)

            # Cast arrays to POINTER(Qnn_Tensor_t) as the CFUNCTYPE expects
            inputs_ptr = ctypes.cast(inputs_arr, ctypes.POINTER(Qnn_Tensor_t))
            outputs_ptr = ctypes.cast(outputs_arr, ctypes.POINTER(Qnn_Tensor_t))

            from .utils_dump import dump_tensors
            # Dump all inputs/outputs for a graph
            dump_tensors(inputs, gi.numInputTensors, kind="INPUTS")
            dump_tensors(outputs, gi.numOutputTensors, kind="OUTPUTS")

            rc = self.qnn_provider.graphExecute(
                gi.graph,
                inputs_ptr, c_uint32(int(gi.numInputTensors)),
                outputs_ptr, c_uint32(int(gi.numOutputTensors)),
                self.profile, c_void_p()
            )
            if rc != 0:
                # Optional: decode backend error
                sm, vm = ErrorHelper.describe(
                    self.qnn_provider.qnn_err_handler, rc)
                ErrorHelper.print("graphExecute", rc, sm, vm)
                raise RuntimeError(f"graphExecute failed rc={rc}")

            print(f"[CALL] graphExecute[{i}] rc={rc} name={graph_name}")
            if rc != 0:
                return rc

            # Guard: output writing
            try:
                io.write_outputs(
                    outputs, gi, graph_name, self.opts.output_dir,
                    output_data_type=self.opts.output_data_type,
                    batch_size=1, num_inputs_populated=1
                )
            except Exception as e:
                print("[ERR] write_outputs failed:", e)
                raise

            io.teardown(inputs, outputs, gi)

        return 0

    def _select_manager_for_model(self, model: Optional[str]) -> GraphContextManager:
        """
        Pick exactly one manager that matches the requested 'model'.
        Matching rule:
          1) Normalize both sides: lowercase and remove all non-alphanumeric chars.
          2) Use substring match on the normalized strings.
        Examples accepted: 'textencoder', 'text_encoder', 'unet', 'vae', full filename, etc.
        """
        if not self._graph_mgrs:
            raise RuntimeError("No GraphContextManager initialized; did start() run?")

        # If no model given and only one manager exists, use it (backward compat).
        if not model:
            if len(self._graph_mgrs) == 1:
                return self._graph_mgrs[0]
            raise ValueError(
                "Multiple models available; please specify model "
                "(e.g., 'vae', 'text_encoder' / 'textencoder', 'unet')."
            )

        def _norm(s: str) -> str:
            return re.sub(r'[^a-z0-9]', '', (s or '').lower())

        key_norm = _norm(model)
        candidates = []
        for mgr in self._graph_mgrs:
            src = getattr(mgr.opts, "retrieve_context", "") or ""
            base = os.path.basename(src)
            base_norm = _norm(base)
            if key_norm and key_norm in base_norm:
                candidates.append(mgr)

        if len(candidates) == 1:
            return candidates[0]
        if len(candidates) == 0:
            raise RuntimeError(
                f"No manager found for model '{model}'. "
                f"Available: {[os.path.basename(getattr(m.opts,'retrieve_context','')) for m in self._graph_mgrs]}"
            )
        raise RuntimeError(
            f"Ambiguous model '{model}'. Matches: "
            f"{[os.path.basename(getattr(m.opts,'retrieve_context','')) for m in candidates]}"
        )

    def execute_graphs(self, model: Optional[str] = None) -> int:
        """
        Execute graphs only for the manager that corresponds to the given model.
        Example models: 'vae', 'text_encoder', 'unet', or a full filename.
        """
        mgr = self._select_manager_for_model(model)
        return self._execute_graphs_for_manager(mgr)
