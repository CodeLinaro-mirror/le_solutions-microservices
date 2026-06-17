# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import ctypes
from ctypes import c_void_p, c_uint32
from typing import Optional
from .options import AppOptions
from .lib_provider import QnnLibrary, QnnProvider, SystemProvider, CORE_LOG_CB, ErrorHelper
from .qnn_types import QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH, Qnn_Tensor_t
from .graph_types import GraphInfo
from .model_interop import QnnModelInterop
from .io_tensor import IOTensor, tv_view
from .system_structs import QnnSystemContext_BinaryInfo_t
from .utils_dump import print


class QnnSampleApp:
    def __init__(self, opts: AppOptions, autoload_libs: bool = True) -> None:
        self.opts = opts
        os.makedirs(self.opts.output_dir, exist_ok=True)

        backend_name = os.path.basename(
            opts.backend_path) if opts.backend_path else "libQnnHtp.so"
        system_name = os.path.basename(
            opts.system_library) if opts.system_library else "libQnnSystem.so"
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

        # Graphs
        self._graphs_ppp = None
        self._graphs_count = 0

        # Model interop
        self._model_interop: Optional[QnnModelInterop] = None

        # Keep system context alive in context-binary path
        self._sys_ctx = None

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
        if not self.opts.op_packages:
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
                  f"path={path} "
                  f"provider={provider} "
                  f"target={target}")
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
        if self.opts.profiling_level == "off":
            return
        lvl_map = {"basic": 1, "detailed": 2}
        lvl = lvl_map.get(self.opts.profiling_level, 1)
        rc = self.qnn_provider.profileCreate(
            self.backend, int(lvl), ctypes.byref(self.profile))
        print(f"[CALL] profileCreate rc={rc} profile=0x{(self.profile.value or 0):016x}")
        if rc != 0:
            raise RuntimeError(f"profileCreate failed rc={rc}")

    def _create_context(self):
        if self.opts.retrieve_context:
            with open(self.opts.retrieve_context, 'rb') as f:
                blob = f.read()
            if not blob:
                raise RuntimeError(f"Empty context binary: {self.opts.retrieve_context}")

            # Build raw uint8_t buffer and keep it alive on self
            BufT = ctypes.c_uint8 * len(blob)
            self._ctx_bin_buf = BufT.from_buffer_copy(blob)     # keep ref
            bin_ptr = ctypes.cast(self._ctx_bin_buf, c_void_p)

            rc = self.qnn_provider.contextCreateFromBinary(
                self.backend,
                self.device,
                ctypes.POINTER(ctypes.POINTER(c_void_p))(),  # configs = NULL
                bin_ptr,
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
            print(f"[CALL] contextCreate rc={rc} "
                  f"context=0x{(self.context.value or 0):016x}")
            if rc != 0:
                raise RuntimeError(f"contextCreate failed rc={rc}")

    def _load_graphs_from_context_binary(self):
        if not self.opts.retrieve_context:
            raise RuntimeError("retrieve_context not set")
        if self.system_provider is None:
            raise RuntimeError(
                "System provider not loaded; set system_library in AppOptions")

        # ---- read blob ----
        with open(self.opts.retrieve_context, "rb") as f:
            blob = f.read()
        if not blob:
            raise RuntimeError(f"Empty context binary: {self.opts.retrieve_context}")

        BufT = ctypes.c_uint8 * len(blob)
        buf_arr = BufT.from_buffer_copy(blob)
        buf_ptr = ctypes.cast(buf_arr, c_void_p)

        # ---- create system context ----
        sys_ctx = c_void_p()

        rc = self.system_provider.systemContextCreate(ctypes.byref(sys_ctx))
        if rc != 0 or not sys_ctx:
            raise RuntimeError(f"systemContextCreate failed rc={rc}")

        self._sys_ctx = sys_ctx

        # ---- prefer getMetadata (modern), fall back to getBinaryInfo if needed ----
        bi_pp = ctypes.POINTER(QnnSystemContext_BinaryInfo_t)()

        bi_size = ctypes.c_size_t(0)
        rc_bi = 0

        rc_bi = self.system_provider.systemContextGetBinaryInfo(
            self._sys_ctx, buf_ptr, ctypes.c_uint64(len(blob)),
            ctypes.byref(bi_pp), ctypes.byref(bi_size)
        )

        have_bi = (rc_bi == 0) and bool(bi_pp)

        if not have_bi:
            # ensure clean free
            try:
                self.system_provider.systemContextFree(self._sys_ctx)
            except Exception:
                pass
            raise RuntimeError(f"Neither metadata (rc={rc_md}) "
                               f"nor binary info (rc={rc_bi}) available")

        BI = bi_pp.contents  # typed; safe
        v = int(BI.version)
        print(f"[DEBUG] BinaryInfo.version={v}")

        # ---- select graphs pointer and count from the correct union member ----
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
            raise RuntimeError(
                "No graphs found in BinaryInfo; check SDK/layout compatibility.")

        print(f"[INFO] Using BinaryInfo: num_graphs={num_graphs}")

        # ---- iterate by-value array QnnSystemContext_GraphInfo_t* ----
        graphinfo_instances, graph_ptrs = [], []
        for i in range(num_graphs):
            gi_src = graphs_ptr[i]  # by-value struct
            vgi = int(gi_src.version)
            if vgi == 1:
                gi_in = gi_src.u.graphInfoV1
            elif vgi == 2:
                gi_in = gi_src.u.graphInfoV2
            elif vgi == 3:
                gi_in = gi_src.u.graphInfoV3
            else:
                raise RuntimeError(
                    f"Graph[{i}] unsupported info version: {vgi}")

            name = gi_in.graphName or b""
            graph_handle = c_void_p()

            rc = self.qnn_provider.graphRetrieve(
                self.context, name, ctypes.byref(graph_handle))

            if rc != 0 or not graph_handle:
                try:
                    gname = name.decode() if name else ""
                except Exception:
                    gname = "<decode error>"
                raise RuntimeError(
                    f"graphRetrieve failed rc={rc} name={gname}")

            gi_local                  = GraphInfo()
            gi_local.graph            = graph_handle
            gi_local.graphName        = name
            gi_local.inputTensors     = gi_in.graphInputs
            gi_local.numInputTensors  = gi_in.numGraphInputs
            gi_local.outputTensors    = gi_in.graphOutputs
            gi_local.numOutputTensors = gi_in.numGraphOutputs

            graphinfo_instances.append(gi_local)
            graph_ptrs.append(ctypes.pointer(gi_local))

            try:
                gname = name.decode() if name else ""
            except Exception:
                gname = "<decode error>"
            print(f"[INFO] Graph[{i}] name='{gname}' "
                  f"inputs={int(gi_in.numGraphInputs)} "
                  f"outputs={int(gi_in.numGraphOutputs)}")

        # ---- build GraphInfo*** and finalize ----
        PP_GraphInfo = ctypes.POINTER(GraphInfo)
        pp_array = (PP_GraphInfo * num_graphs)()

        for i in range(num_graphs):
            pp_array[i] = graph_ptrs[i]

        pp_ptr = ctypes.cast(pp_array, ctypes.POINTER(PP_GraphInfo))
        self._graphs_ppp = ctypes.pointer(pp_ptr)
        self._graphs_count = num_graphs

        self._gi_instances = graphinfo_instances
        self._gi_ptrs      = graph_ptrs
        self._gi_pp_array  = pp_array
        self._gi_pp_ptr    = pp_ptr

        self._finalize_graph_list()

    def _check_finalize_capability(self) -> int:
        support_finalize_deserialized_graph = int(
            QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH)
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
            rc_f = self.qnn_provider.graphFinalize(
                gi.graph, self.profile, c_void_p())
            name = gi.graphName.decode() if gi.graphName else ''
            print(f'[CALL] graphFinalize[{i}] rc={rc_f} name={name}')
            if rc_f != 0:
                raise RuntimeError(f'graphFinalize[{i}] failed rc={rc_f}')

    # --- lifecycle ---

    def start(self) -> None:
        if self.qnn_provider is None:
            self._load_libs()

        self._init_logging()
        self._init_backend()
        self._register_op_packages()
        self._create_device()
        self._init_profiling()
        self._create_context()
        self._compose_and_finalize_graphs()

    def run_once(self) -> None:
        self.execute_graphs()

        if self.opts.save_context:
            size = ctypes.c_size_t(0)
            rc = self.qnn_provider.contextGetBinarySize(
                self.context, ctypes.byref(size))
            print(f"[CALL] contextGetBinarySize rc={rc} size={size.value}")
            if rc != 0:
                raise RuntimeError(f"contextGetBinarySize failed rc={rc}")

            buf = (ctypes.c_uint8 * size.value)()
            written = ctypes.c_size_t(0)
            rc = self.qnn_provider.contextGetBinary(self.context, ctypes.cast(
                buf, c_void_p), size.value, ctypes.byref(written))
            print(f"[CALL] contextGetBinary rc={rc} written={written.value}")
            if rc != 0:
                raise RuntimeError(f"contextGetBinary failed rc={rc}")

            out_path = os.path.join(
                self.opts.output_dir, f"{self.opts.save_context}.bin")
            with open(out_path, "wb") as f:
                f.write(bytes(buf)[: written.value])

            print(f"[INFO] Saved context binary to {out_path}")

    def stop(self) -> None:
        # free system context (context-binary path)
        try:
            if getattr(self, "_sys_ctx", None):
                rc = self.system_provider.systemContextFree(self._sys_ctx)
                print(f"[CALL] systemContextFree rc={rc}")
        except Exception:
            pass
        self._sys_ctx = None

        try:
            if self.context and self.context.value:
                rc = self.qnn_provider.contextFree(self.context, self.profile)
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

    def run(self) -> int:
        try:
            self.start()
            for _ in range(max(1, int(self.opts.num_inferences))):
                self.run_once()
            return 0
        except Exception as e:
            print("[FATAL]", e)
            return 2
        finally:
            self.stop()

    # --- compose or context-loader selector ---
    def _compose_and_finalize_graphs(self) -> None:
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

            print(f"self._graphs_ppp::{self._graphs_ppp}")
            print(f"self._graphs_count::{self._graphs_count}")

            print(
                f"[INFO] composeGraphs produced {self._graphs_count} graph(s).")

            self._finalize_graph_list()

            return

        if self.opts.retrieve_context:
            print(
                "[INFO] No model .so provided; loading graphs from context binary via System API")
            self._load_graphs_from_context_binary()
            print(
                f"[INFO] Loaded {self._graphs_count} graph(s) from context binary.")
            return

        print(
            "[FATAL] Neither --model_path nor --retrieve_context was provided; cannot build graphs.")

        raise RuntimeError(
            "No model_path (.so) or retrieve_context (.bin) specified. "
            "Provide one of them in AppOptions to build graphs."
        )

    def _ensure_system_interface_is_sane(self):
        # Check that systemProvider exists and function pointers look plausible.
        if self.system_provider is None:
            raise RuntimeError(
                "System provider not loaded; set system_library in AppOptions")

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
            if isinstance(p, int):
                addr = p
            else:
                addr = int(p)
            if addr < 0x1000:
                raise RuntimeError(
                    f"System interface pointer for {name} looks invalid: 0x{addr:016x}. "
                    "This usually indicates a QNN_SYSTEM_INTERFACE_VER_TYPE layout mismatch.\n"
                    "Please ensure lib_provider.py's QNN_SYSTEM_INTERFACE_VER_TYPE "
                    "fields match SDK exactly."
                )

    # --- execute using prepared GraphInfo*** ---

    def execute_graphs(self) -> int:
        import ctypes
        try:
            if not self._graphs_ppp or not bool(self._graphs_ppp):
                raise RuntimeError(
                    "No graphs loaded (self._graphs_ppp is null). "
                    "Ensure compose/context loader ran.")

            base_pp = self._graphs_ppp.contents
            arr = ctypes.cast(base_pp, ctypes.POINTER(
                ctypes.POINTER(GraphInfo)))

            io = IOTensor()
            for i in range(self._graphs_count):
                gi = arr[i].contents
                graph_name = gi.graphName.decode() if gi.graphName else ""

                inputs, outputs, in_buf_refs, out_buf_refs = io.setup_input_output(
                    gi, system_provider=self.system_provider
                )

                # Guard: input population
                try:
                    if self.opts.input_list_paths:
                        io.populate_inputs(inputs, gi, self.opts.input_list_paths,
                                           input_data_type=self.opts.input_data_type)
                except Exception as e:
                    print("[ERR] populate_inputs failed:", e)
                    raise

                def _assert_non_null_buffers(gi, inputs, outputs):
                    for i in range(gi.numInputTensors):
                        _, tv = tv_view(inputs[i])
                        ptr = ctypes.cast(tv.clientBuf.data,c_void_p).value or 0

                        if ptr == 0:
                            name = tv.name.decode() if tv.name else f"Input_{i}"
                            raise RuntimeError(f"NULL input buffer at {i} ({name})")
                        tv.memType = c_uint32(0)

                    for i in range(gi.numOutputTensors):
                        _, tv = tv_view(outputs[i])
                        ptr = ctypes.cast(tv.clientBuf.data,c_void_p).value or 0
                        if ptr == 0:
                            name = tv.name.decode() if tv.name else f"Output_{i}"
                            raise RuntimeError(f"NULL output buffer at {i} ({name})")
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

                inputs_arr = _as_tensor_array(inputs,  gi.numInputTensors)
                outputs_arr = _as_tensor_array(outputs, gi.numOutputTensors)

                # Cast arrays to POINTER(Qnn_Tensor_t) as the CFUNCTYPE expects
                inputs_ptr = ctypes.cast(
                    inputs_arr,  ctypes.POINTER(Qnn_Tensor_t))
                outputs_ptr = ctypes.cast(
                    outputs_arr, ctypes.POINTER(Qnn_Tensor_t))


                from .utils_dump import dump_tensor, dump_tensors

                # Dump all inputs/outputs for a graph
                dump_tensors(inputs, gi.numInputTensors, kind="INPUTS")
                dump_tensors(outputs, gi.numOutputTensors, kind="OUTPUTS")


                print("rc = self.qnn_provider.graphExecute(")
                print(f"\tgi.graph::{gi.graph}")
                print(f"\tinputs_ptr::{inputs_ptr}")
                print(f"\tc_uint32(int(gi.numInputTensors))::{c_uint32(int(gi.numInputTensors))}")
                print(f"\toutputs_ptr::{outputs_ptr}")
                print(f"\tc_uint32(int(gi.numOutputTensors))::{c_uint32(int(gi.numOutputTensors))}")
                print(f"\tself.profile::{self.profile}")
                print(f"\tc_void_p()::{c_void_p()}")
                print(")")

                rc = self.qnn_provider.graphExecute(
                    gi.graph,
                    inputs_ptr,   c_uint32(int(gi.numInputTensors)),
                    outputs_ptr,  c_uint32(int(gi.numOutputTensors)),
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
        except Exception as e:
            raise RuntimeError("[ERR] graphExecute loop failed:", e)
        return 0
