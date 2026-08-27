# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import ctypes
import numpy as np

from .graph_types import (
    element_count,
    DATA_TYPE_SIZE,                # legacy sizes (kept for fallback)
)
from .qnn_types import (
    # Tensor & quantization types
    Qnn_Tensor_t, Qnn_QuantizeParams_t, qnn_dtype_size_bytes,
    # Bit-coded QNN datatypes (SIZE suffix in low byte)
    QNN_DATATYPE_UINT_8 as QNN_DT_U8,
    QNN_DATATYPE_UINT_16 as QNN_DT_U16,
    QNN_DATATYPE_UINT_32 as QNN_DT_U32,
    QNN_DATATYPE_UINT_64 as QNN_DT_U64,
    QNN_DATATYPE_INT_8 as QNN_DT_I8,
    QNN_DATATYPE_INT_16 as QNN_DT_I16,
    QNN_DATATYPE_INT_32 as QNN_DT_I32,
    QNN_DATATYPE_INT_64 as QNN_DT_I64,
    QNN_DATATYPE_BOOL_8 as QNN_DT_B8,
    QNN_DATATYPE_FLOAT_16 as QNN_DT_F16,
    QNN_DATATYPE_FLOAT_32 as QNN_DT_F32,
    QNN_DATATYPE_FLOAT_64 as QNN_DT_F64,
    QNN_DATATYPE_UFIXED_POINT_8 as QNN_DT_UFIXED8,
    QNN_DATATYPE_UFIXED_POINT_16 as QNN_DT_UFIXED16,
    QNN_DATATYPE_UFIXED_POINT_32 as QNN_DT_UFIXED32,
    # Quantization encodings
    QNN_QUANTIZATION_ENCODING_SCALE_OFFSET as ENC_SCALE,
    QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET as ENC_AXIS,
    QNN_QUANTIZATION_ENCODING_BW_SCALE_OFFSET as ENC_BW,
    QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET as ENC_BW_AXIS,
    QNN_QUANTIZATION_ENCODING_BLOCK as ENC_BLOCK,
    QNN_QUANTIZATION_ENCODING_BLOCKWISE_EXPANSION as ENC_BLOCKWISE,
    QNN_QUANTIZATION_ENCODING_VECTOR as ENC_VECTOR,
)

from .qnn_types import Qnn_Tensor_t as Qnn_Tensor_t_
from .qnn_types import Qnn_QuantizeParams_t as Qnn_QuantizeParams_t_
from .qnn_types import Qnn_TensorRetrieveRaw_t   # only for completeness

from .qnn_types import Qnn_Tensor_t  # keep single symbol
from .qnn_types import Qnn_QuantizeParams_t

# I/O enums
QNN_TENSOR_TYPE_APP_WRITE = 0
QNN_TENSOR_TYPE_APP_READ  = 1
QNN_TENSOR_DATA_FORMAT_DENSE = 0
QNN_TENSORMEMTYPE_RAW = 0

# --- Env toggles for v1/v2 tensor heads ---
_FORCE_V1 = os.getenv("QNN_TENSOR_FORCE_V1", "1").strip().lower() in ("1", "true", "yes", "on")
_ALLOW_V2 = os.getenv("QNN_TENSOR_ALLOW_V2", "0").strip().lower() in ("1", "true", "yes", "on")

def _safe_cstr(c_char_p) -> str:
    try:
        return c_char_p.decode() if c_char_p else ""
    except Exception:
        return ""

def _elem_size_from_dtype(dtype_code: int) -> int:
    # prefer bit-coded fallback; DATA_TYPE_SIZE is legacy
    return int(qnn_dtype_size_bytes(int(dtype_code)))

def _numpy_dtype_from_qnn(dtype_code: int):
    """
    Map QNN bit-coded dtype to a NumPy dtype for raw/native views.
    """
    m = {
        QNN_DT_U8:  np.uint8,
        QNN_DT_U16: np.uint16,
        QNN_DT_U32: np.uint32,
        QNN_DT_U64: np.uint64,
        QNN_DT_I8:  np.int8,
        QNN_DT_I16: np.int16,
        QNN_DT_I32: np.int32,
        QNN_DT_I64: np.int64,
        QNN_DT_B8:  np.uint8,   # bool8 container
        QNN_DT_F16: np.float16,
        QNN_DT_F32: np.float32,
        QNN_DT_F64: np.float64,
        # UFIXED containers map to unsigned ints for native view:
        QNN_DT_UFIXED8:  np.uint8,
        QNN_DT_UFIXED16: np.uint16,
        QNN_DT_UFIXED32: np.uint32,
    }
    return m.get(int(dtype_code), None)

def _looks_plausible_view(tv) -> bool:
    try:
        _ = _safe_cstr(tv.name)
    except Exception:
        return False
    r = int(getattr(tv, "rank", 0))
    if r < 0 or r > 16:
        return False
    if r > 0 and not bool(getattr(tv, "dimensions", None)):
        return False
    dt = int(getattr(tv, "dataType", 0))
    if dt < 0 or dt > 0xFFFF:
        return False
    mt = int(getattr(tv, "memType", 0))
    if mt < 0 or mt > 16:
        return False
    return True

def tv_view(t: Qnn_Tensor_t):
    """
    Version-safe union selector for Qnn_Tensor_t.
    Prefer v1 head; allow v2 only if explicitly enabled and looks sane.
    """
    ver = int(getattr(t, "version", 1))
    norm = ver if ver < 10 else ver // 10
    if norm <= 1 and hasattr(t, "v1"):
        return ver, t.v1
    if norm >= 2 and hasattr(t, "v1"):
        if _ALLOW_V2 and hasattr(t, "v2"):
            tv2 = t.v2
            if _looks_plausible_view(tv2):
                return ver, tv2
        return ver, t.v1
    if hasattr(t, "v2"):
        return ver, t.v2
    if hasattr(t, "v1"):
        return ver, t.v1
    raise RuntimeError(f"Unsupported Qnn_Tensor_t (version={ver}): no union members found")

# ---- Quantization helpers (existing) ----
def _float_to_tfn(out_arr, in_float, offset, scale, bitwidth):
    true_max = (2 ** bitwidth) - 1
    enc_min = float(offset) * float(scale)
    enc_max = (true_max + float(offset)) * float(scale)
    rng = enc_max - enc_min if enc_max != enc_min else 1.0
    q = np.round(true_max * (in_float - enc_min) / rng)
    q = np.clip(q, 0, true_max).astype(out_arr.dtype)
    out_arr[:] = q
    return out_arr

def _float_to_bw_scale_offset(out_arr: np.ndarray, in_float: np.ndarray,
                              offset: int, scale: float, bitwidth: int):
    true_max = (1 << int(bitwidth)) - 1
    enc_min = float(offset) * float(scale)
    enc_max = (true_max + float(offset)) * float(scale)
    rng = enc_max - enc_min if enc_max != enc_min else 1.0
    q = np.round(true_max * (in_float - enc_min) / rng)
    q = np.clip(q, 0, true_max).astype(out_arr.dtype)
    out_arr[:] = q
    return out_arr

def _dequantize_scalar_encoding(native: np.ndarray, qparams: Qnn_QuantizeParams_t) -> np.ndarray:
    scale = qparams.scaleOffsetEncoding.scale
    offset = qparams.scaleOffsetEncoding.offset
    out = (native.astype(np.float64) + float(offset)) * float(scale)
    return out.astype(np.float32, copy=False)

def _dequantize_axis_encoding(native: np.ndarray, qparams: Qnn_QuantizeParams_t, rank: int, elem_count: int) -> np.ndarray:
    axis_enc = qparams.axisScaleOffsetEncoding
    num = int(axis_enc.numScaleOffsets)
    if num <= 0 or not bool(axis_enc.scaleOffset):
        return native.astype(np.float32, copy=False)
    scales = np.empty(num, dtype=np.float32)
    offsets = np.empty(num, dtype=np.int32)
    for i in range(num):
        so = axis_enc.scaleOffset[i]
        scales[i] = float(so.scale)
        offsets[i] = int(so.offset)
    flat = native.astype(np.float32, copy=False)
    if rank > 0 and elem_count > 0 and elem_count % num == 0:
        reps = elem_count // num
        scales_full = np.repeat(scales, reps)
        offsets_full = np.repeat(offsets, reps)
    else:
        reps = (len(flat) + num - 1) // num
        scales_full = np.tile(scales, reps)[:len(flat)]
        offsets_full = np.tile(offsets, reps)[:len(flat)]
    return (flat + offsets_full.astype(np.float32)) * scales_full

def _dequantize_bw_axis_encoding(native: np.ndarray, qparams: Qnn_QuantizeParams_t, rank: int, elem_count: int) -> np.ndarray:
    enc = qparams.bwAxisScaleOffsetEncoding
    num = int(enc.numElements)
    if num <= 0 or not bool(enc.scales) or not bool(enc.offsets):
        return native.astype(np.float32, copy=False)
    scales = np.ctypeslib.as_array(enc.scales, shape=(num,)).astype(np.float32, copy=False)
    offsets = np.ctypeslib.as_array(enc.offsets, shape=(num,)).astype(np.int32, copy=False)
    flat = native.astype(np.float32, copy=False)
    if rank > 0 and elem_count > 0 and elem_count % num == 0:
        reps = elem_count // num
        scales_full = np.repeat(scales, reps)
        offsets_full = np.repeat(offsets, reps)
    else:
        reps = (len(flat) + num - 1) // num
        scales_full = np.tile(scales, reps)[:len(flat)]
        offsets_full = np.tile(offsets, reps)[:len(flat)]
    return (flat + offsets_full.astype(np.float32)) * scales_full

def _cast_to_float(native_arr):
    return native_arr.astype(np.float32)

class IOTensor:
    def setup_input_output(self, gi, system_provider=None):
        input_buf_refs, output_buf_refs = [], []
        inputs = (Qnn_Tensor_t * gi.numInputTensors)()
        outputs = (Qnn_Tensor_t * gi.numOutputTensors)()

        # Inputs
        for i in range(gi.numInputTensors):
            inputs[i] = gi.inputTensors[i]
            _, tv = tv_view(inputs[i])
            tv.type = ctypes.c_uint32(QNN_TENSOR_TYPE_APP_WRITE)
            tv.dataFormat = ctypes.c_uint32(QNN_TENSOR_DATA_FORMAT_DENSE)
            tv.memType = ctypes.c_uint32(QNN_TENSORMEMTYPE_RAW)
            tv.clientBuf.data = ctypes.c_void_p(0)
            tv.clientBuf.dataSize = ctypes.c_uint32(0)

        # Outputs
        for i in range(gi.numOutputTensors):
            outputs[i] = gi.outputTensors[i]
            _, tv = tv_view(outputs[i])
            tv.type = ctypes.c_uint32(QNN_TENSOR_TYPE_APP_READ)
            tv.dataFormat = ctypes.c_uint32(QNN_TENSOR_DATA_FORMAT_DENSE)
            tv.memType = ctypes.c_uint32(QNN_TENSORMEMTYPE_RAW)

            # Size by either shape or system footprint
            size = 0
            try:
                # Prefer bit-coded size suffix
                elem_sz = _elem_size_from_dtype(tv.dataType)
                elems = int(element_count(tv.dimensions, int(tv.rank))) if bool(tv.dimensions) and int(tv.rank) > 0 else 0
                if elems > 0:
                    size = elems * elem_sz
            except Exception:
                size = 0

            if size <= 0:
                if system_provider is None or not hasattr(system_provider, "systemTensorGetMemoryFootprint"):
                    nm = _safe_cstr(tv.name) or f"Output_{i}"
                    raise RuntimeError(f"No shape-based size and no system footprint provider for {nm}")
                footprint = ctypes.c_uint64(0)
                rc_fp = system_provider.systemTensorGetMemoryFootprint(outputs[i], ctypes.byref(footprint))
                if rc_fp != 0 or footprint.value == 0:
                    nm = _safe_cstr(tv.name) or f"Output_{i}"
                    raise RuntimeError(f"systemTensorGetMemoryFootprint failed rc={rc_fp} for {nm}")
                size = int(footprint.value)

            buf = ctypes.create_string_buffer(size)
            tv.clientBuf.data = ctypes.cast(buf, ctypes.c_void_p)
            tv.clientBuf.dataSize = ctypes.c_uint32(size)
            output_buf_refs.append(buf)

        return inputs, outputs, input_buf_refs, output_buf_refs

    def _tensor_is_quantized(self, tv) -> bool:
        enc = int(tv.quantizeParams.quantizationEncoding)
        return enc in (ENC_SCALE, ENC_AXIS, ENC_BW, ENC_BW_AXIS, ENC_BLOCK, ENC_BLOCKWISE, ENC_VECTOR)

    def populate_inputs(self, inputs, gi, input_list_path: str, input_data_type: str = 'float'):
        columns, name_to_idx = _read_input_list(input_list_path)
        if len(columns) != int(gi.numInputTensors):
            print(f"[WARN] Input columns ({len(columns)}) != model inputs ({int(gi.numInputTensors)}) — using name/index mapping with clip/pad.")

        if not hasattr(self, "_in_buf_refs"): self._in_buf_refs = []
        if not hasattr(self, "_in_dims_refs"): self._in_dims_refs = []

        for i in range(gi.numInputTensors):
            t = inputs[i]
            _, tv = tv_view(t)
            name = _safe_cstr(tv.name)
            col_idx = name_to_idx.get(name, i)
            if col_idx < 0 or col_idx >= len(columns):
                raise RuntimeError(f"No input column available for tensor '{name}' (index {i}); have {len(columns)} column(s).")
            file_paths = columns[col_idx]

            elem_size = _elem_size_from_dtype(tv.dataType)
            rank = int(tv.rank)
            has_dims = bool(tv.dimensions) and rank > 0
            shape_elems = element_count(tv.dimensions, rank) if has_dims else 0
            shape_elems = int(shape_elems) if shape_elems else 0

            # --- FLOAT input path (we will quantize/cast as needed) ---
            if input_data_type.lower() == 'float':
                vals = []
                for path in file_paths:
                    arr = np.fromfile(path, dtype=np.float32)
                    if arr.size:
                        vals.append(arr)
                batch_all = np.concatenate(vals) if vals else np.empty(0, dtype=np.float32)

                dt = int(tv.dataType)
                enc = int(tv.quantizeParams.quantizationEncoding)

                if self._tensor_is_quantized(tv):
                    # Quantized input expected -> convert float -> quantized integer/ufixed
                    native_dtype = _numpy_dtype_from_qnn(dt) or {1: np.uint8, 2: np.uint16, 4: np.uint32, 8: np.uint64}.get(elem_size, np.uint8)
                    out = np.empty(batch_all.size, dtype=native_dtype)
                    if enc == ENC_BW:
                        bw = int(getattr(tv.quantizeParams.bwScaleOffsetEncoding, "bitwidth", 0) or 0)
                        if bw and bw != elem_size * 8:
                            _float_to_bw_scale_offset(out, batch_all,
                                                      tv.quantizeParams.bwScaleOffsetEncoding.offset,
                                                      tv.quantizeParams.bwScaleOffsetEncoding.scale,
                                                      bw)
                        else:
                            _float_to_tfn(out, batch_all,
                                          tv.quantizeParams.bwScaleOffsetEncoding.offset,
                                          tv.quantizeParams.bwScaleOffsetEncoding.scale,
                                          elem_size * 8)
                    else:
                        # Default to scalar encoding (most runtime inputs use this)
                        _float_to_tfn(out, batch_all,
                                      tv.quantizeParams.scaleOffsetEncoding.offset,
                                      tv.quantizeParams.scaleOffsetEncoding.scale,
                                      elem_size * 8)
                else:
                    # Non-quantized -> cast to model's float dtype
                    qnn_np_dtype = _numpy_dtype_from_qnn(dt) or np.float32
                    out = batch_all.astype(qnn_np_dtype)

                # Truncate/pad raw byte length to expected
                expected_size = (shape_elems * elem_size) if shape_elems > 0 else int(out.size) * elem_size
                out_bytes = out.tobytes()
                if len(out_bytes) > expected_size:
                    out_bytes = out_bytes[:expected_size]
                elif len(out_bytes) < expected_size:
                    out_bytes = out_bytes + b"\x00" * (expected_size - len(out_bytes))

                buf = ctypes.create_string_buffer(expected_size)
                ctypes.memmove(buf, out_bytes, expected_size)
                tv.memType = ctypes.c_uint32(QNN_TENSORMEMTYPE_RAW)
                tv.clientBuf.data = ctypes.cast(buf, ctypes.c_void_p)
                tv.clientBuf.dataSize = ctypes.c_uint32(expected_size)
                self._in_buf_refs.append(buf)

                if shape_elems == 0:
                    elem_count = expected_size // elem_size
                    DimsArrT = ctypes.c_uint32 * 1
                    dims_buf = DimsArrT(elem_count)
                    tv.rank = ctypes.c_uint32(1)
                    tv.dimensions = ctypes.cast(dims_buf, ctypes.POINTER(ctypes.c_uint32))
                    self._in_dims_refs.append(dims_buf)

            # --- RAW/native input path (unchanged logic) ---
            else:
                raw = bytearray()
                for path in file_paths:
                    with open(path, 'rb') as fh:
                        raw.extend(fh.read())
                if shape_elems > 0:
                    expected_size = shape_elems * elem_size
                    if len(raw) > expected_size:
                        raw = raw[:expected_size]
                    elif len(raw) < expected_size:
                        raw.extend(b"\x00" * (expected_size - len(raw)))
                else:
                    elem_count_guess = len(raw) // elem_size
                    expected_size = elem_count_guess * elem_size
                    raw = raw[:expected_size]

                buf = ctypes.create_string_buffer(expected_size)
                ctypes.memmove(buf, bytes(raw), expected_size)
                tv.memType = ctypes.c_uint32(QNN_TENSORMEMTYPE_RAW)
                tv.clientBuf.data = ctypes.cast(buf, ctypes.c_void_p)
                tv.clientBuf.dataSize = ctypes.c_uint32(expected_size)
                self._in_buf_refs.append(buf)

                if shape_elems == 0:
                    elem_count = expected_size // elem_size
                    DimsArrT = ctypes.c_uint32 * 1
                    dims_buf = DimsArrT(elem_count)
                    tv.rank = ctypes.c_uint32(1)
                    tv.dimensions = ctypes.cast(dims_buf, ctypes.POINTER(ctypes.c_uint32))
                    self._in_dims_refs.append(dims_buf)

    def write_outputs(self, outputs, gi, graph_name: str, out_dir: str,
                      output_data_type: str = 'float_only', batch_size: int = 1,
                      num_inputs_populated: int = 1):
        base = out_dir
        if int(num_inputs_populated) > 1 and graph_name:
            base = os.path.join(base, graph_name)
        os.makedirs(base, exist_ok=True)

        res_dirs = [os.path.join(base, f"Result_{i}") for i in range(num_inputs_populated)]
        for d in res_dirs:
            os.makedirs(d, exist_ok=True)

        for oi in range(gi.numOutputTensors):
            t = outputs[oi]
            _, tv = tv_view(t)
            name = _safe_cstr(tv.name) or f"Output_{oi}"
            float_fname = f"{name}.raw"
            native_fname = f"{name}_native.raw"

            elem_cnt = int(element_count(tv.dimensions, tv.rank)) if bool(tv.dimensions) and int(tv.rank) > 0 else 0
            bytes_len = int(tv.clientBuf.dataSize)
            dt = int(tv.dataType)
            enc = int(tv.quantizeParams.quantizationEncoding)

            # Build native view using exact container dtype for this QNN dtype
            qnn_np_dtype = _numpy_dtype_from_qnn(dt) or np.uint8
            buf = (ctypes.c_uint8 * bytes_len).from_address(tv.clientBuf.data)
            native = np.frombuffer(bytes(buf), dtype=qnn_np_dtype)

            # Dequantize only if a quantization encoding is present
            if enc in (ENC_BW_AXIS, ENC_AXIS, ENC_BW, ENC_SCALE, ENC_BLOCK, ENC_BLOCKWISE, ENC_VECTOR):
                if enc == ENC_BW_AXIS:
                    fbuf = _dequantize_bw_axis_encoding(native, tv.quantizeParams, int(tv.rank), elem_cnt).astype(np.float32, copy=False)
                elif enc == ENC_AXIS:
                    fbuf = _dequantize_axis_encoding(native, tv.quantizeParams, int(tv.rank), elem_cnt).astype(np.float32, copy=False)
                else:
                    fbuf = _dequantize_scalar_encoding(native, tv.quantizeParams).astype(np.float32, copy=False)
            else:
                fbuf = _cast_to_float(native)

            # Write float output (default)
            chunk = elem_cnt // max(1, batch_size) if elem_cnt else 0
            for bi, d in enumerate(res_dirs):
                fpath = os.path.join(d, float_fname)
                if chunk > 0:
                    fbuf[bi * chunk:(bi + 1) * chunk].tofile(fpath)
                else:
                    fbuf.tofile(fpath)

            # Optionally also dump native (disabled by default)
            if output_data_type in ('native_only', 'float_and_native'):
                chunk_bytes = bytes_len // max(1, batch_size)
                b = bytes(buf)
                for bi, d in enumerate(res_dirs):
                    start = bi * chunk_bytes
                    end = start + chunk_bytes
                    out_name = native_fname if (output_data_type == 'native_only') else float_fname
                    with open(os.path.join(d, out_name), 'wb') as fh:
                        fh.write(b[start:end])

    def teardown(self, inputs, outputs, gi):
        pass

def _read_input_list(input_list_path: str):
    lines = []
    with open(input_list_path, 'r') as fh:
        for line in fh:
            s = line.strip()
            if not s or s.startswith('#') or s.startswith('%'):
                continue
            lines.append(s)
    name_to_idx = {}
    if lines:
        tokens = lines[0].split()
        for idx, tok in enumerate(tokens):
            if ':=' in tok:
                name, _ = tok.split(':=', 1)
                name_to_idx[name] = idx
    columns = []
    for line in lines:
        toks = line.split()
        paths = [t.split(':=', 1)[-1] if ':=' in t else t for t in toks]
        for ci, p in enumerate(paths):
            if ci >= len(columns):
                columns.append([])
            columns[ci].append(p)
    return columns, name_to_idx
