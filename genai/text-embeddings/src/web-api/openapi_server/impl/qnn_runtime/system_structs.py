# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import ctypes
from ctypes import (
    c_char_p,
    c_uint32,
    c_uint64,
    c_void_p,
    POINTER,
    Structure,
    Union,
)

from .qnn_types import Qnn_Tensor_t


# ----------------------------------------------------------------------
# QNN version triplet (major, minor, patch)
# ----------------------------------------------------------------------
class Qnn_Version_t(Structure):
    _fields_ = [
        ("major", c_uint32),
        ("minor", c_uint32),
        ("patch", c_uint32),
    ]


# ----------------------------------------------------------------------
# GraphInfo (V1 / V2 / V3) — header-accurate layouts
# ----------------------------------------------------------------------

class QnnSystemContext_GraphInfoV1_t(Structure):
    _fields_ = [
        ("graphName", c_char_p),
        ("numGraphInputs", c_uint32),
        ("graphInputs", POINTER(Qnn_Tensor_t)),
        ("numGraphOutputs", c_uint32),
        ("graphOutputs", POINTER(Qnn_Tensor_t)),
    ]


class QnnSystemContext_GraphInfoV2_t(Structure):
    _fields_ = [
        ("graphName", c_char_p),
        ("numGraphInputs", c_uint32),
        ("graphInputs", POINTER(Qnn_Tensor_t)),
        ("numGraphOutputs", c_uint32),
        ("graphOutputs", POINTER(Qnn_Tensor_t)),
        ("numUpdateableTensors", c_uint32),
        ("updateableTensors", POINTER(Qnn_Tensor_t)),
    ]


class QnnSystemContext_GraphInfoV3_t(Structure):
    _fields_ = [
        ("graphName", c_char_p),
        ("numGraphInputs", c_uint32),
        ("graphInputs", POINTER(Qnn_Tensor_t)),
        ("numGraphOutputs", c_uint32),
        ("graphOutputs", POINTER(Qnn_Tensor_t)),
        ("numUpdateableTensors", c_uint32),
        ("updateableTensors", POINTER(Qnn_Tensor_t)),
        ("graphBlobInfoSize", c_uint32),
        ("graphBlobInfo", c_void_p),     # void*
        ("startOpIndex", c_uint32),
        ("endOpIndex", c_uint32),
    ]


class _GraphInfoUnion(Union):
    _fields_ = [
        ("graphInfoV1", QnnSystemContext_GraphInfoV1_t),
        ("graphInfoV2", QnnSystemContext_GraphInfoV2_t),
        ("graphInfoV3", QnnSystemContext_GraphInfoV3_t),
    ]


class QnnSystemContext_GraphInfo_t(Structure):
    _fields_ = [
        ("version", c_uint32),   # QnnSystemContext_GraphInfoVersion_t
        ("u", _GraphInfoUnion),
    ]


# ----------------------------------------------------------------------
# BinaryInfo (V1 / V2 / V3) — header-accurate layouts
# ----------------------------------------------------------------------

class QnnSystemContext_BinaryInfoV1_t(Structure):
    _fields_ = [
        # Identification & versions
        ("backendId", c_uint32),
        ("buildId", c_char_p),
        ("coreApiVersion", Qnn_Version_t),
        ("backendApiVersion", Qnn_Version_t),
        ("socVersion", c_char_p),
        ("hwInfoBlobVersion", Qnn_Version_t),
        ("contextBlobVersion", Qnn_Version_t),

        # Hardware blob + context blob sizes/pointers
        ("hwInfoBlobSize", c_uint32),
        ("hwInfoBlob", c_void_p),       # void*
        ("contextBlobSize", c_uint64),  # uint64_t

        # Context tensors
        ("numContextTensors", c_uint32),
        ("contextTensors", POINTER(Qnn_Tensor_t)),

        # Graphs list (BY VALUE ARRAY)
        ("numGraphs", c_uint32),
        ("graphs", POINTER(QnnSystemContext_GraphInfo_t)),
    ]


class QnnSystemContext_BinaryInfoV2_t(Structure):
    _fields_ = [
        # Identification & versions
        ("backendId", c_uint32),
        ("buildId", c_char_p),
        ("coreApiVersion", Qnn_Version_t),
        ("backendApiVersion", Qnn_Version_t),
        ("socVersion", c_char_p),
        ("hwInfoBlobVersion", Qnn_Version_t),
        ("contextBlobVersion", Qnn_Version_t),

        # Hardware blob + context blob sizes/pointers
        ("hwInfoBlobSize", c_uint32),
        ("hwInfoBlob", c_void_p),       # void*
        ("contextBlobSize", c_uint64),  # uint64_t

        # Context tensors
        ("numContextTensors", c_uint32),
        ("contextTensors", POINTER(Qnn_Tensor_t)),

        # Graphs list (BY VALUE ARRAY)
        ("numGraphs", c_uint32),
        ("graphs", POINTER(QnnSystemContext_GraphInfo_t)),

        # Device information (opaque)
        ("platformInfo", c_void_p),     # QnnDevice_PlatformInfo_t*
    ]


class QnnSystemContext_BinaryInfoV3_t(Structure):
    _fields_ = [
        # Identification & versions (note: V3 drops hwInfoBlobVersion fields present in V1/V2)
        ("backendId", c_uint32),
        ("buildId", c_char_p),
        ("coreApiVersion", Qnn_Version_t),
        ("backendApiVersion", Qnn_Version_t),
        ("socVersion", c_char_p),
        ("contextBlobVersion", Qnn_Version_t),

        # Opaque backend context blob
        ("contextBlobSize", c_uint64),  # uint64_t

        # Context tensors
        ("numContextTensors", c_uint32),
        ("contextTensors", POINTER(Qnn_Tensor_t)),

        # Graphs list (BY VALUE ARRAY)
        ("numGraphs", c_uint32),
        ("graphs", POINTER(QnnSystemContext_GraphInfo_t)),

        # Device information (opaque)
        ("platformInfo", c_void_p),     # QnnDevice_PlatformInfo_t*

        # Context metadata (opaque) + size
        ("contextMetadataSize", c_uint32),
        ("contextMetadata", c_void_p),  # void*

        # SoC identifier
        ("socModel", c_uint32),
    ]


class _BinaryInfoUnion(Union):
    _fields_ = [
        ("contextBinaryInfoV1", QnnSystemContext_BinaryInfoV1_t),
        ("contextBinaryInfoV2", QnnSystemContext_BinaryInfoV2_t),
        ("contextBinaryInfoV3", QnnSystemContext_BinaryInfoV3_t),
    ]


class QnnSystemContext_BinaryInfo_t(Structure):
    _fields_ = [
        ("version", c_uint32),  # QnnSystemContext_BinaryInfoVersion_t
        ("u", _BinaryInfoUnion),
    ]
