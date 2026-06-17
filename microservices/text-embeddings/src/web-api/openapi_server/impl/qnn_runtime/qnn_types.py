# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from ctypes import (
    c_void_p, c_uint8, c_uint16, c_uint32, c_uint64, c_int32, c_float, c_char_p,
    POINTER, Structure, Union, CFUNCTYPE
)

# ----------------------------------------------------------------------
# Property groups & capability keys
# ----------------------------------------------------------------------
QNN_PROPERTY_GROUP_CORE   = 0x00000001
QNN_PROPERTY_GROUP_GRAPH  = QNN_PROPERTY_GROUP_CORE + 300
QNN_PROPERTY_GRAPH_SUPPORT_FINALIZE_DESERIALIZED_GRAPH = QNN_PROPERTY_GROUP_GRAPH + 18

QNN_PROPERTY_SUPPORTED     = 0
QNN_PROPERTY_NOT_SUPPORTED = 1  # symbolic non-zero fallback

# ---------- Common typedefs / enums (ABI-safe as 32-bit) ----------

Qnn_TensorVersion_t        = c_uint32
Qnn_TensorType_t           = c_uint32
Qnn_TensorDataFormat_t     = c_uint32
Qnn_DataType_t             = c_uint32
Qnn_TensorMemType_t        = c_uint32
Qnn_Definition_t           = c_uint32
Qnn_QuantizationEncoding_t = c_uint32
Qnn_SparseLayoutType_t     = c_uint32

# QNN_TENSOR_DATA_FORMAT_* (subset we may read/write)
QNN_TENSOR_DATA_FORMAT_DENSE = c_uint32(0).value

# QNN_TENSOR_MEMTYPE_*
QNN_TENSORMEMTYPE_RAW       = c_uint32(0).value
QNN_TENSORMEMTYPE_MEMHANDLE = c_uint32(1).value
QNN_TENSORMEMTYPE_RETRIEVE_RAW = c_uint32(2).value

# QNN_DATATYPE_* (bit-encoded; last byte hints element size: 0x08->1B, 0x16->2B, 0x32->4B, 0x64->8B)
QNN_DATATYPE_INT_8   = 0x0008
QNN_DATATYPE_INT_16  = 0x0016
QNN_DATATYPE_INT_32  = 0x0032
QNN_DATATYPE_INT_64  = 0x0064
QNN_DATATYPE_UINT_8  = 0x0108
QNN_DATATYPE_UINT_16 = 0x0116
QNN_DATATYPE_UINT_32 = 0x0132
QNN_DATATYPE_UINT_64 = 0x0164
QNN_DATATYPE_FLOAT_16 = 0x0216
QNN_DATATYPE_FLOAT_32 = 0x0232
QNN_DATATYPE_FLOAT_64 = 0x0264
QNN_DATATYPE_UFIXED_POINT_4  = 0x0404
QNN_DATATYPE_UFIXED_POINT_8  = 0x0408
QNN_DATATYPE_UFIXED_POINT_16 = 0x0416
QNN_DATATYPE_UFIXED_POINT_32 = 0x0432
QNN_DATATYPE_BOOL_8 = 0x0508

# QNN_QUANTIZATION_ENCODING_*
QNN_QUANTIZATION_ENCODING_SCALE_OFFSET         = 0
QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET    = 1
QNN_QUANTIZATION_ENCODING_BW_SCALE_OFFSET      = 2
QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET = 3
QNN_QUANTIZATION_ENCODING_BLOCK                = 4
QNN_QUANTIZATION_ENCODING_BLOCKWISE_EXPANSION  = 5
QNN_QUANTIZATION_ENCODING_VECTOR               = 6
QNN_QUANTIZATION_ENCODING_FLOAT_BLOCK          = 7

# ---------- Scalar/quantization encodings (value/ptr per header) ----------

class Qnn_ScaleOffset_t(Structure):
    _fields_ = [
        ("scale",  c_float),
        ("offset", c_int32),
    ]

class Qnn_BwScaleOffset_t(Structure):
    _fields_ = [
        ("bitwidth", c_uint32),
        ("scale",    c_float),
        ("offset",   c_int32),
    ]

class Qnn_AxisScaleOffset_t(Structure):
    _fields_ = [
        ("axis",           c_int32),
        ("numScaleOffsets", c_uint32),
        ("scaleOffset",    POINTER(Qnn_ScaleOffset_t)),
    ]

class Qnn_BwAxisScaleOffset_t(Structure):
    _fields_ = [
        ("bitwidth",    c_uint32),
        ("axis",        c_int32),
        ("numElements", c_uint32),
        ("scales",      POINTER(c_float)),
        ("offsets",     POINTER(c_int32)),
    ]

class Qnn_BlockEncoding_t(Structure):
    _fields_ = [
        ("blockSize",  POINTER(c_uint32)),      # array [rank]
        ("scaleOffset", POINTER(Qnn_ScaleOffset_t)),  # array [numBlocks]
    ]

# enum Qnn_BlockwiseExpansionBlockScaleStorageType_t (partial)
QNN_BLOCKWISE_EXPANSION_BITWIDTH_SCALE_STORAGE_8  = 0
QNN_BLOCKWISE_EXPANSION_BITWIDTH_SCALE_STORAGE_16 = 1

class Qnn_BlockwiseExpansion_t(Structure):
    _fields_ = [
        ("axis",                 c_int32),
        ("scaleOffsets",         POINTER(Qnn_ScaleOffset_t)),  # axis-sized
        ("numBlocksPerAxis",     c_uint32),
        ("blockScaleBitwidth",   c_uint32),
        ("blockScaleStorageType", c_uint32),  # enum, 8 or 16
        # union { uint8_t* blocksScale8; uint16_t* blocksScale16; }
        ("_blocks_ptr",          c_void_p),
    ]

class Qnn_VectorEncoding_t(Structure):
    _fields_ = [
        ("bwAxisScaleOffset", Qnn_BwAxisScaleOffset_t),
        ("rowsPerBlock",     c_uint32),
        ("columnsPerBlock",  c_uint32),
        ("vectorDimension",  c_uint8),
        ("vectorStride",     c_uint8),
        ("indexBitwidth",    c_uint8),
    ]
    _pack_ = 0  # default alignment

class Qnn_FloatScaleOffset_t(Structure):
    _fields_ = [
        ("scale",  c_float),
        ("offset", c_float),
    ]

class Qnn_FloatBlockEncoding_t(Structure):
    _fields_ = [
        ("blockSize",        POINTER(c_uint32)),
        ("floatScaleOffset", POINTER(Qnn_FloatScaleOffset_t)),
    ]

class _Qnn_QParams_Union(Union):
    _fields_ = [
        ("scaleOffsetEncoding",      Qnn_ScaleOffset_t),       # value
        ("axisScaleOffsetEncoding",  Qnn_AxisScaleOffset_t),   # value
        ("bwScaleOffsetEncoding",    Qnn_BwScaleOffset_t),     # value
        ("bwAxisScaleOffsetEncoding",Qnn_BwAxisScaleOffset_t), # value
        ("blockEncoding",            Qnn_BlockEncoding_t),     # value
        ("blockwiseExpansion",       POINTER(Qnn_BlockwiseExpansion_t)),  # pointer
        ("vectorEncoding",           POINTER(Qnn_VectorEncoding_t)),      # pointer
        ("floatBlockEncoding",       Qnn_FloatBlockEncoding_t),           # value
    ]

class Qnn_QuantizeParams_t(Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("encodingDefinition",  Qnn_Definition_t),
        ("quantizationEncoding", Qnn_QuantizationEncoding_t),
        ("u",                   _Qnn_QParams_Union),
    ]

# ---------- Tensor memory / retrieve-raw ----------

class Qnn_ClientBuffer_t(Structure):
    _fields_ = [
        ("data",     c_void_p),
        ("dataSize", c_uint32),
    ]

# Handles and error type as opaque pointers for ctypes purposes
Qnn_ContextHandle_t = c_void_p
Qnn_GraphHandle_t   = c_void_p
Qnn_ErrorHandle_t   = c_void_p
Qnn_MemHandle_t     = c_void_p

# typedef Qnn_ErrorHandle_t (*Qnn_GetTensorRawDataFn_t)(context, graph, id, clientBuf)
Qnn_GetTensorRawDataFn_t = CFUNCTYPE(
    Qnn_ErrorHandle_t, Qnn_ContextHandle_t, Qnn_GraphHandle_t, c_uint64, POINTER(Qnn_ClientBuffer_t)
)

# typedef Qnn_ErrorHandle_t (*Qnn_FreeTensorRawDataFn_t)(context, graph, id)
Qnn_FreeTensorRawDataFn_t = CFUNCTYPE(
    Qnn_ErrorHandle_t, Qnn_ContextHandle_t, Qnn_GraphHandle_t, c_uint64
)

class Qnn_TensorRetrieveRaw_t(Structure):
    _fields_ = [
        ("getTensorData",  Qnn_GetTensorRawDataFn_t),
        ("freeTensorData", Qnn_FreeTensorRawDataFn_t),
    ]

# ---------- Sparse params (used in V2) ----------

class Qnn_SparseLayoutHybridCoo_t(Structure):
    _fields_ = [
        ("numSpecifiedElements", c_uint32),
        ("numSparseDimensions",  c_uint32),
    ]

class _Qnn_SparseParams_Union(Union):
    _fields_ = [
        ("hybridCoo", Qnn_SparseLayoutHybridCoo_t),
    ]

class Qnn_SparseParams_t(Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("type", Qnn_SparseLayoutType_t),
        ("u",    _Qnn_SparseParams_Union),
    ]

# ---------- Tensor V1 / V2 / versioned wrapper ----------

class _Qnn_TensorMemory_Union(Union):
    _fields_ = [
        ("clientBuf", Qnn_ClientBuffer_t),
        ("memHandle", Qnn_MemHandle_t),
    ]

class Qnn_TensorV1_t(Structure):
    _anonymous_ = ("mem",)
    _fields_ = [
        ("id",          c_uint32),
        ("name",        c_char_p),
        ("type",        Qnn_TensorType_t),
        ("dataFormat",  Qnn_TensorDataFormat_t),
        ("dataType",    Qnn_DataType_t),
        ("quantizeParams", Qnn_QuantizeParams_t),
        ("rank",        c_uint32),
        ("dimensions",  POINTER(c_uint32)),
        ("memType",     Qnn_TensorMemType_t),
        ("mem",         _Qnn_TensorMemory_Union),
    ]

class _Qnn_TensorMemoryV2_Union(Union):
    _fields_ = [
        ("clientBuf",  Qnn_ClientBuffer_t),
        ("memHandle",  Qnn_MemHandle_t),
        ("retrieveRaw", POINTER(Qnn_TensorRetrieveRaw_t)),
    ]

class Qnn_TensorV2_t(Structure):
    _anonymous_ = ("mem",)
    _fields_ = [
        ("id",          c_uint32),
        ("name",        c_char_p),
        ("type",        Qnn_TensorType_t),
        ("dataFormat",  Qnn_TensorDataFormat_t),
        ("dataType",    Qnn_DataType_t),
        ("quantizeParams", Qnn_QuantizeParams_t),
        ("rank",        c_uint32),
        ("dimensions",  POINTER(c_uint32)),
        ("memType",     Qnn_TensorMemType_t),
        ("mem",         _Qnn_TensorMemoryV2_Union),
        ("isDynamicDimensions", POINTER(c_uint8)),   # uint8_t* [rank] or NULL
        ("sparseParams",       Qnn_SparseParams_t),  # by value
        ("isProduced",         c_uint8),             # boolean / undefined if not supported
    ]

class _Qnn_Tensor_Union(Union):
    _fields_ = [
        ("v1", Qnn_TensorV1_t),
        ("v2", Qnn_TensorV2_t),
    ]

class Qnn_Tensor_t(Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("version", Qnn_TensorVersion_t),
        ("u",       _Qnn_Tensor_Union),
    ]

# ---------- Convenience: dtype size fallback for bit-coded types ----------

# Lowest byte (0x..08, 0x..16, 0x..32, 0x..64) encodes element size.
# We expose a helper you can import from io_tensor if DATA_TYPE_SIZE isn't available.
_SIZE_SUFFIX_TO_BYTES = {
    0x04: 1,  # treat 4-bit packed as 1 byte container when sizing buffers
    0x08: 1,
    0x16: 2,
    0x32: 4,
    0x64: 8,
}

def qnn_dtype_size_bytes(dtype_code: int) -> int:
    """Derive element size from QNN bit-coded data type (e.g., 0x0232 -> 4 bytes)."""
    suffix = int(dtype_code) & 0xFF
    return _SIZE_SUFFIX_TO_BYTES.get(suffix, 4)
