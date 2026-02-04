# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import ctypes
from ctypes import (
    c_int, c_uint32, c_uint8, c_size_t, c_void_p, c_char_p,
    POINTER, byref,
)
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

# Opaque handles / typedefs
LiteRtStatus = c_int
LiteRtEnvironment = c_void_p
LiteRtModel = c_void_p
LiteRtOptions = c_void_p
LiteRtCompiledModel = c_void_p
LiteRtSignature = c_void_p
LiteRtTensor = c_void_p
LiteRtRankedTensorType = c_void_p
LiteRtTensorBuffer = c_void_p
LiteRtTensorBufferRequirements = c_void_p
LiteRtParamIndex = c_uint32
LiteRtTensorBufferLockMode = c_int

# Hardware masks
HW_CPU = 1
HW_GPU = 2
HW_NPU = 4
HW_ALL = HW_CPU | HW_GPU | HW_NPU


def _proto_eager(dll: ctypes.CDLL) -> Dict[str, Any]:
    f: Dict[str, Any] = {}

    def bind(name: str, restype, argtypes):
        fn = getattr(dll, name)
        fn.restype = restype
        fn.argtypes = argtypes
        f[name] = fn

    # Model / Options / Environment
    bind("LiteRtCreateModelFromBuffer", LiteRtStatus, [c_void_p, c_size_t, POINTER(c_void_p)])
    bind("LiteRtDestroyModel", None, [c_void_p])

    bind("LiteRtCreateOptions", LiteRtStatus, [POINTER(c_void_p)])
    bind("LiteRtDestroyOptions", None, [c_void_p])

    bind("LiteRtCreateEnvironment", LiteRtStatus, [c_int, POINTER(c_void_p), POINTER(c_void_p)])
    bind("LiteRtDestroyEnvironment", None, [c_void_p])

    bind("LiteRtSetOptionsHardwareAccelerators", LiteRtStatus, [c_void_p, c_int])
    bind("LiteRtAddOpaqueOptions", LiteRtStatus, [c_void_p, c_void_p])

    # Compiled model
    bind("LiteRtCreateCompiledModel", LiteRtStatus, [c_void_p, c_void_p, c_void_p, POINTER(c_void_p)])
    bind("LiteRtDestroyCompiledModel", None, [c_void_p])

    # Signature / tensors
    bind("LiteRtGetModelSignature", LiteRtStatus, [c_void_p, LiteRtParamIndex, POINTER(c_void_p)])
    bind("LiteRtGetNumSignatureInputs", LiteRtStatus, [c_void_p, POINTER(LiteRtParamIndex)])
    bind("LiteRtGetNumSignatureOutputs", LiteRtStatus, [c_void_p, POINTER(LiteRtParamIndex)])
    bind("LiteRtGetSignatureInputTensorByIndex", LiteRtStatus, [c_void_p, LiteRtParamIndex, POINTER(c_void_p)])
    bind("LiteRtGetSignatureOutputTensorByIndex", LiteRtStatus, [c_void_p, LiteRtParamIndex, POINTER(c_void_p)])
    bind("LiteRtGetRankedTensorType", LiteRtStatus, [c_void_p, POINTER(c_void_p)])

    # Buffer requirements / buffers
    bind("LiteRtGetCompiledModelInputBufferRequirements", LiteRtStatus,
         [c_void_p, LiteRtParamIndex, LiteRtParamIndex, POINTER(c_void_p)])
    bind("LiteRtGetCompiledModelOutputBufferRequirements", LiteRtStatus,
         [c_void_p, LiteRtParamIndex, LiteRtParamIndex, POINTER(c_void_p)])
    bind("LiteRtGetTensorBufferRequirementsBufferSize", LiteRtStatus, [c_void_p, POINTER(c_size_t)])
    bind("LiteRtGetTensorBufferRequirementsSupportedTensorBufferType", LiteRtStatus, [c_void_p, c_int, POINTER(c_int)])
    bind("LiteRtCreateManagedTensorBuffer", LiteRtStatus,
         [c_void_p, c_int, POINTER(c_void_p), c_size_t, POINTER(c_void_p)])

    # Lock/unlock
    bind("LiteRtLockTensorBuffer", LiteRtStatus, [c_void_p, POINTER(c_void_p), c_int])
    bind("LiteRtUnlockTensorBuffer", LiteRtStatus, [c_void_p])

    # Optional packed size
    try:
        bind("LiteRtGetTensorBufferPackedSize", LiteRtStatus, [c_void_p, POINTER(c_size_t)])
    except AttributeError:
        pass

    bind("LiteRtDestroyTensorBuffer", None, [c_void_p])

    # Run (sync)
    bind("LiteRtRunCompiledModel", LiteRtStatus,
         [c_void_p, LiteRtParamIndex,
          c_size_t, POINTER(c_void_p),
          c_size_t, POINTER(c_void_p)])

    # Optional status string
    try:
        bind("LiteRtGetStatusString", c_char_p, [LiteRtStatus])
    except AttributeError:
        pass

    return f


def _status_to_str(fns: Dict[str, Any], status: int) -> str:
    get_str = fns.get("LiteRtGetStatusString")
    if get_str:
        try:
            s = get_str(LiteRtStatus(int(status)))
            return s.decode("utf-8") if s else f"LiteRtStatus({int(status)})"
        except Exception:
            pass
    return f"LiteRtStatus({int(status)})"


def _check_ok(fns: Dict[str, Any], status: int, where: str, verbose: bool = True):
    if int(status) != 0:
        raise RuntimeError(f"{where} failed: {_status_to_str(fns, status)}")
    if verbose:
        print(f"{where} success: {_status_to_str(fns, status)}")


class LiteRtLibProvider:
    def __init__(self, runtime_lib: str = "libLiteRt.so"):
        self.runtime_lib = runtime_lib
        self.dll = ctypes.CDLL(runtime_lib, mode=ctypes.RTLD_GLOBAL)
        self.fns = _proto_eager(self.dll)

    def get(self, name: str):
        return self.fns.get(name)

    def bind_optional(self, name: str, restype, argtypes):
        try:
            fn = getattr(self.dll, name)
            fn.restype = restype
            fn.argtypes = argtypes
            self.fns[name] = fn
            return fn
        except AttributeError:
            return None

    def has_symbol(self, name: str) -> bool:
        return getattr(self.dll, name, None) is not None


class QualcommOptions:
    """
    Qualcomm options wrapper.
    Your libLiteRt.so exports these symbols (Create/SetUseHtpPreference/SetHtpPerformanceMode, etc.).
    """
    def __init__(self, provider: LiteRtLibProvider):
        self.p = provider
        self.fns = provider.fns
        self.handle = c_void_p(None)

        self._q_create = self.p.bind_optional(
            "LiteRtQualcommOptionsCreate", LiteRtStatus, [POINTER(c_void_p)]
        )
        self._set_use_htp = self.p.bind_optional(
            "LiteRtQualcommOptionsSetUseHtpPreference", LiteRtStatus, [c_void_p, ctypes.c_bool]
        )
        self._set_perf = self.p.bind_optional(
            "LiteRtQualcommOptionsSetHtpPerformanceMode", LiteRtStatus, [c_void_p, c_int]
        )
        self._add_opaque = self.p.get("LiteRtAddOpaqueOptions")

    def create(self):
        if not self._q_create:
            raise RuntimeError("LiteRtQualcommOptionsCreate not exported by this runtime.")
        st = self._q_create(byref(self.handle))
        _check_ok(self.fns, st, "LiteRtQualcommOptionsCreate")

    def set_use_htp_preference(self, use_htp: bool = True):
        if self.handle.value is None:
            raise RuntimeError("QualcommOptions: create() must be called first.")
        if not self._set_use_htp:
            return
        st = self._set_use_htp(self.handle, bool(use_htp))
        _check_ok(self.fns, st, "LiteRtQualcommOptionsSetUseHtpPreference")

    def set_htp_performance_mode(self, mode: int):
        if self.handle.value is None:
            raise RuntimeError("QualcommOptions: create() must be called first.")
        if not self._set_perf:
            return
        st = self._set_perf(self.handle, int(mode))
        _check_ok(self.fns, st, "LiteRtQualcommOptionsSetHtpPerformanceMode")

    def attach_to_litert_options(self, options_handle: LiteRtOptions):
        if self.handle.value is None:
            raise RuntimeError("QualcommOptions: create() must be called first.")
        if not self._add_opaque:
            raise RuntimeError("LiteRtAddOpaqueOptions not exported by runtime.")
        st = self._add_opaque(options_handle, self.handle)
        _check_ok(self.fns, st, "LiteRtAddOpaqueOptions(Qualcomm)")


@dataclass(frozen=True)
class IoRequirements:
    num_inputs: int
    num_outputs: int
    input_req_sizes: Tuple[int, ...]
    output_req_sizes: Tuple[int, ...]


class LiteRtInterpreter:
    def __init__(
        self,
        model_path: str,
        runtime_lib: str = "libLiteRt.so",
        debug: bool = False,
        hw_mask: int = HW_CPU,
        compile_on_init: bool = True,
    ):
        self.debug = debug
        self.p = LiteRtLibProvider(runtime_lib)
        self.fns = self.p.fns

        self.environment = LiteRtEnvironment(None)
        self.model = LiteRtModel(None)
        self.options = LiteRtOptions(None)
        self.compiled = LiteRtCompiledModel(None)

        self._in_bufs: List[LiteRtTensorBuffer] = []
        self._out_bufs: List[LiteRtTensorBuffer] = []
        self._in_req_sizes: List[int] = []
        self._out_req_sizes: List[int] = []
        self._closed = False

        self._npu_compiled = False
        self._last_hw_mask = int(hw_mask)

        with open(model_path, "rb") as f:
            data = f.read()
        buf = (c_uint8 * len(data)).from_buffer_copy(data)

        st = self.fns["LiteRtCreateModelFromBuffer"](
            ctypes.cast(buf, c_void_p),
            c_size_t(len(data)),
            byref(self.model),
        )
        _check_ok(self.fns, st, "LiteRtCreateModelFromBuffer")

        st = self.fns["LiteRtCreateOptions"](byref(self.options))
        _check_ok(self.fns, st, "LiteRtCreateOptions")

        null_opts = POINTER(c_void_p)()
        st = self.fns["LiteRtCreateEnvironment"](c_int(0), null_opts, byref(self.environment))
        _check_ok(self.fns, st, "LiteRtCreateEnvironment")

        st = self.fns["LiteRtSetOptionsHardwareAccelerators"](self.options, c_int(int(hw_mask)))
        _check_ok(self.fns, st, "LiteRtSetOptionsHardwareAccelerators")

        if compile_on_init:
            self._recompile_or_raise()

    def capabilities(self) -> Dict[str, bool]:
        return {
            "has_qualcomm_options_create": self.p.has_symbol("LiteRtQualcommOptionsCreate"),
            "has_qualcomm_use_htp": self.p.has_symbol("LiteRtQualcommOptionsSetUseHtpPreference"),
            "has_qualcomm_perf_mode": self.p.has_symbol("LiteRtQualcommOptionsSetHtpPerformanceMode"),
            "has_tensorbuffer_packed_size": self.fns.get("LiteRtGetTensorBufferPackedSize") is not None,
        }

    def npu_compiled(self) -> bool:
        return bool(self._npu_compiled)

    def _create_compiled(self) -> LiteRtCompiledModel:
        tmp = LiteRtCompiledModel(None)
        st = self.fns["LiteRtCreateCompiledModel"](self.environment, self.model, self.options, byref(tmp))
        _check_ok(self.fns, st, "LiteRtCreateCompiledModel")
        return tmp

    def _recompile_or_raise(self):
        new_compiled = self._create_compiled()
        old = self.compiled
        self.compiled = new_compiled
        try:
            if old and self.fns.get("LiteRtDestroyCompiledModel"):
                self.fns["LiteRtDestroyCompiledModel"](old)
        except Exception:
            pass

    def _reset_options(self, hw_mask: int):
        try:
            if self.options and self.fns.get("LiteRtDestroyOptions"):
                self.fns["LiteRtDestroyOptions"](self.options)
        except Exception:
            pass

        self.options = LiteRtOptions(None)
        st = self.fns["LiteRtCreateOptions"](byref(self.options))
        _check_ok(self.fns, st, "LiteRtCreateOptions(reset)")

        st = self.fns["LiteRtSetOptionsHardwareAccelerators"](self.options, c_int(int(hw_mask)))
        _check_ok(self.fns, st, "LiteRtSetOptionsHardwareAccelerators(reset)")

        self._last_hw_mask = int(hw_mask)

    def enable_npu_htp(self, qualcomm_opts: Optional[QualcommOptions], require_npu: bool = False) -> bool:
        baseline_compiled = self.compiled
        baseline_npu = self._npu_compiled
        baseline_hw = self._last_hw_mask

        # Try NPU-only first
        try:
            self._reset_options(HW_NPU)
            self._recompile_or_raise()
            npu_possible = True
        except Exception:
            npu_possible = False
            self.compiled = baseline_compiled
            self._npu_compiled = baseline_npu
            self._last_hw_mask = baseline_hw

        if npu_possible:
            try:
                if qualcomm_opts is not None:
                    self._reset_options(HW_NPU)
                    qualcomm_opts.attach_to_litert_options(self.options)
                    self._recompile_or_raise()
                self._npu_compiled = True
                return True
            except Exception:
                # Still allow plain HW_NPU
                self._reset_options(HW_NPU)
                self._recompile_or_raise()
                self._npu_compiled = True
                return True

        if require_npu:
            raise RuntimeError("HTP/NPU requested but HW_NPU compilation is not possible on this system.")

        # Fallback
        try:
            self._reset_options(HW_ALL)
            self._recompile_or_raise()
            self._npu_compiled = False
            return False
        except Exception:
            self._reset_options(HW_CPU)
            self._recompile_or_raise()
            self._npu_compiled = False
            return False

    def allocate_tensors(self, signature_index: int = 0):
        sig = LiteRtSignature(None)
        st = self.fns["LiteRtGetModelSignature"](self.model, LiteRtParamIndex(signature_index), byref(sig))
        _check_ok(self.fns, st, "LiteRtGetModelSignature")

        n_in = LiteRtParamIndex(0)
        st = self.fns["LiteRtGetNumSignatureInputs"](sig, byref(n_in))
        _check_ok(self.fns, st, "LiteRtGetNumSignatureInputs")

        n_out = LiteRtParamIndex(0)
        st = self.fns["LiteRtGetNumSignatureOutputs"](sig, byref(n_out))
        _check_ok(self.fns, st, "LiteRtGetNumSignatureOutputs")

        self._in_bufs, self._out_bufs = [], []
        self._in_req_sizes, self._out_req_sizes = [], []

        for i in range(int(n_in.value)):
            t = LiteRtTensor(None)
            st = self.fns["LiteRtGetSignatureInputTensorByIndex"](sig, LiteRtParamIndex(i), byref(t))
            _check_ok(self.fns, st, "LiteRtGetSignatureInputTensorByIndex")

            ty = LiteRtRankedTensorType(None)
            st = self.fns["LiteRtGetRankedTensorType"](t, byref(ty))
            _check_ok(self.fns, st, "LiteRtGetRankedTensorType")

            req = LiteRtTensorBufferRequirements(None)
            st = self.fns["LiteRtGetCompiledModelInputBufferRequirements"](
                self.compiled, LiteRtParamIndex(signature_index), LiteRtParamIndex(i), byref(req)
            )
            _check_ok(self.fns, st, "LiteRtGetCompiledModelInputBufferRequirements")

            buf_sz = c_size_t(0)
            st = self.fns["LiteRtGetTensorBufferRequirementsBufferSize"](req, byref(buf_sz))
            _check_ok(self.fns, st, "LiteRtGetTensorBufferRequirementsBufferSize")
            self._in_req_sizes.append(int(buf_sz.value))

            buf_type = c_int(0)
            st = self.fns["LiteRtGetTensorBufferRequirementsSupportedTensorBufferType"](req, c_int(0), byref(buf_type))
            _check_ok(self.fns, st, "LiteRtGetTensorBufferRequirementsSupportedTensorBufferType")

            tb = LiteRtTensorBuffer(None)
            st = self.fns["LiteRtCreateManagedTensorBuffer"](self.environment, buf_type, byref(ty), buf_sz, byref(tb))
            _check_ok(self.fns, st, "LiteRtCreateManagedTensorBuffer(input)")
            self._in_bufs.append(tb)

        for i in range(int(n_out.value)):
            t = LiteRtTensor(None)
            st = self.fns["LiteRtGetSignatureOutputTensorByIndex"](sig, LiteRtParamIndex(i), byref(t))
            _check_ok(self.fns, st, "LiteRtGetSignatureOutputTensorByIndex")

            ty = LiteRtRankedTensorType(None)
            st = self.fns["LiteRtGetRankedTensorType"](t, byref(ty))
            _check_ok(self.fns, st, "LiteRtGetRankedTensorType")

            req = LiteRtTensorBufferRequirements(None)
            st = self.fns["LiteRtGetCompiledModelOutputBufferRequirements"](
                self.compiled, LiteRtParamIndex(signature_index), LiteRtParamIndex(i), byref(req)
            )
            _check_ok(self.fns, st, "LiteRtGetCompiledModelOutputBufferRequirements")

            buf_sz = c_size_t(0)
            st = self.fns["LiteRtGetTensorBufferRequirementsBufferSize"](req, byref(buf_sz))
            _check_ok(self.fns, st, "LiteRtGetTensorBufferRequirementsBufferSize")
            self._out_req_sizes.append(int(buf_sz.value))

            buf_type = c_int(0)
            st = self.fns["LiteRtGetTensorBufferRequirementsSupportedTensorBufferType"](req, c_int(0), byref(buf_type))
            _check_ok(self.fns, st, "LiteRtGetTensorBufferRequirementsSupportedTensorBufferType")

            tb = LiteRtTensorBuffer(None)
            st = self.fns["LiteRtCreateManagedTensorBuffer"](self.environment, buf_type, byref(ty), buf_sz, byref(tb))
            _check_ok(self.fns, st, "LiteRtCreateManagedTensorBuffer(output)")
            self._out_bufs.append(tb)

    def get_io_requirements(self) -> IoRequirements:
        return IoRequirements(
            num_inputs=len(self._in_bufs),
            num_outputs=len(self._out_bufs),
            input_req_sizes=tuple(self._in_req_sizes),
            output_req_sizes=tuple(self._out_req_sizes),
        )

    def write_input_bytes(self, index: int, data: bytes, lock_mode: int = 1):
        req = self._in_req_sizes[index]
        if len(data) > req:
            raise ValueError(f"Input[{index}] too large: {len(data)} > {req}")
        if len(data) < req:
            data = data + (b"\x00" * (req - len(data)))

        ptr = c_void_p(None)
        st = self.fns["LiteRtLockTensorBuffer"](self._in_bufs[index], byref(ptr), LiteRtTensorBufferLockMode(lock_mode))
        _check_ok(self.fns, st, "LiteRtLockTensorBuffer(input)")
        ctypes.memmove(ptr, data, len(data))
        st = self.fns["LiteRtUnlockTensorBuffer"](self._in_bufs[index])
        _check_ok(self.fns, st, "LiteRtUnlockTensorBuffer(input)")

    def read_output_bytes(self, index: int, lock_mode: int = 0) -> bytes:
        get_packed = self.fns.get("LiteRtGetTensorBufferPackedSize")
        if get_packed:
            sz = c_size_t(0)
            st = get_packed(self._out_bufs[index], byref(sz))
            _check_ok(self.fns, st, "LiteRtGetTensorBufferPackedSize")
            size = int(sz.value)
        else:
            size = self._out_req_sizes[index]

        ptr = c_void_p(None)
        st = self.fns["LiteRtLockTensorBuffer"](self._out_bufs[index], byref(ptr), LiteRtTensorBufferLockMode(lock_mode))
        _check_ok(self.fns, st, "LiteRtLockTensorBuffer(output)")
        data = ctypes.string_at(ptr, size) if size else b""
        st = self.fns["LiteRtUnlockTensorBuffer"](self._out_bufs[index])
        _check_ok(self.fns, st, "LiteRtUnlockTensorBuffer(output)")
        return data

    def invoke(self, signature_index: int = 0):
        in_arr = (LiteRtTensorBuffer * len(self._in_bufs))(*self._in_bufs)
        out_arr = (LiteRtTensorBuffer * len(self._out_bufs))(*self._out_bufs)
        st = self.fns["LiteRtRunCompiledModel"](
            self.compiled,
            LiteRtParamIndex(signature_index),
            c_size_t(len(self._in_bufs)), in_arr,
            c_size_t(len(self._out_bufs)), out_arr,
        )
        _check_ok(self.fns, st, "LiteRtRunCompiledModel")

    def close(self):
        if self._closed:
            return
        self._closed = True

        destroy_tb = self.fns.get("LiteRtDestroyTensorBuffer")
        if destroy_tb:
            for tb in self._in_bufs:
                try:
                    if tb:
                        destroy_tb(tb)
                except Exception:
                    pass
            for tb in self._out_bufs:
                try:
                    if tb:
                        destroy_tb(tb)
                except Exception:
                    pass

        self._in_bufs, self._out_bufs = [], []
        self._in_req_sizes, self._out_req_sizes = [], []

        for destroy_name, attr in [
            ("LiteRtDestroyCompiledModel", "compiled"),
            ("LiteRtDestroyEnvironment", "environment"),
            ("LiteRtDestroyOptions", "options"),
            ("LiteRtDestroyModel", "model"),
        ]:
            h = getattr(self, attr, None)
            try:
                fn = self.fns.get(destroy_name)
                if fn and h:
                    fn(h)
            except Exception:
                pass
            setattr(self, attr, None)

    def __del__(self):
        try:
            import sys
            if getattr(sys, "is_finalizing", lambda: False)():
                return
        except Exception:
            pass
