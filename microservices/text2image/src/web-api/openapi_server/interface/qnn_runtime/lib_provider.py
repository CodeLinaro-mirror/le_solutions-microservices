# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import ctypes
from ctypes import (
    c_void_p, c_uint64, c_uint32, c_int, c_char_p, POINTER, Structure, CFUNCTYPE,
    c_size_t
)
from .system_structs import QnnSystemContext_BinaryInfo_t
from .qnn_types import Qnn_Tensor_t
from .utils_dump import print


# =========================================================================================
# 1) Low-level QNN structs & prototypes (header-accurate fragments)
# =========================================================================================
class Qnn_Version_t(Structure):
    _fields_ = [("major", c_uint32), ("minor", c_uint32), ("patch", c_uint32)]


class Qnn_ApiVersion_t(Structure):
    _fields_ = [("coreApiVersion",    Qnn_Version_t),
                ("backendApiVersion", Qnn_Version_t)]

# Important
# Please do not change the sequence in the list!
# The order of the elements matters!
# Мust remain as described in the QnnInterface.h QNN_INTERFACE_VER_TYPE structure!
class QNN_INTERFACE_VER_TYPE(Structure):
    """Typed function table: only the entries we call/inspect; keep others as c_void_p."""
    _fields_ = [
        # Backend
        ("propertyHasCapability",               c_void_p),
        ("backendCreate",                       c_void_p),
        ("backendSetConfig",                    c_void_p),
        ("backendGetApiVersion",                c_void_p),
        ("backendGetBuildId",                   c_void_p),
        ("backendRegisterOpPackage",            c_void_p),
        ("backendGetSupportedOperations",       c_void_p),
        ("backendValidateOpConfig",             c_void_p),
        ("backendFree",                         c_void_p),
        # Context (trimmed)
        ("contextCreate",                       c_void_p),
        ("contextSetConfig",                    c_void_p),
        ("contextGetBinarySize",                c_void_p),
        ("contextGetBinary",                    c_void_p),
        ("contextCreateFromBinary",             c_void_p),
        ("contextFree",                         c_void_p),
        # Graph (trimmed)
        ("graphCreate",                         c_void_p),
        ("graphCreateSubgraph",                 c_void_p),
        ("graphSetConfig",                      c_void_p),
        ("graphAddNode",                        c_void_p),
        ("graphFinalize",                       c_void_p),
        ("graphRetrieve",                       c_void_p),
        ("graphExecute",                        c_void_p),
        ("graphExecuteAsync",                   c_void_p),
        # Tensor
        ("tensorCreateContextTensor",           c_void_p),
        ("tensorCreateGraphTensor",             c_void_p),
        # Logging
        ("logCreate",                           c_void_p),
        ("logSetLogLevel",                      c_void_p),
        ("logFree",                             c_void_p),
        # Profiling (trimmed)
        ("profileCreate",                       c_void_p),
        ("profileSetConfig",                    c_void_p),
        ("profileGetEvents",                    c_void_p),
        ("profileGetSubEvents",                 c_void_p),
        ("profileGetEventData",                 c_void_p),
        ("profileGetExtendedEventData",         c_void_p),
        ("profileFree",                         c_void_p),
        # Memory
        ("memRegister",                         c_void_p),
        ("memDeRegister",                       c_void_p),
        # Device
        ("deviceGetPlatformInfo",               c_void_p),
        ("deviceFreePlatformInfo",              c_void_p),
        ("deviceGetInfrastructure",             c_void_p),
        ("deviceCreate",                        c_void_p),
        ("deviceSetConfig",                     c_void_p),
        ("deviceGetInfo",                       c_void_p),
        ("deviceFree",                          c_void_p),
        # Signal
        ("signalCreate",                        c_void_p),
        ("signalSetConfig",                     c_void_p),
        ("signalTrigger",                       c_void_p),
        ("signalFree",                          c_void_p),
        # Error handling
        ("errorGetMessage",                     c_void_p),
        ("errorGetVerboseMessage",              c_void_p),
        ("errorFreeVerboseMessage",             c_void_p),

        ("graphPrepareExecutionEnvironment",    c_void_p),
        ("graphReleaseExecutionEnvironment",    c_void_p),
        ("graphGetProperty",                    c_void_p),
        ("contextValidateBinary",               c_void_p),
        ("contextCreateFromBinaryWithSignal",   c_void_p),
        ("contextCreateFromBinaryListAsync",    c_void_p),
        ("tensorUpdateGraphTensors",            c_void_p),
        ("tensorUpdateContextTensors",          c_void_p),
        ("contextGetBinarySectionSize",         c_void_p),
        ("contextGetBinarySection",             c_void_p),
        ("contextApplyBinarySection",           c_void_p),
        ("backendGetProperty",                  c_void_p),
        ("contextGetProperty",                  c_void_p),
        ("contextGetIncrementalBinary",         c_void_p),
        ("contextReleaseIncrementalBinary",     c_void_p),
        ("contextFinalize",                     c_void_p),
        ("globalConfigSet",                     c_void_p),
        ("contextCreateFromBinaryWithCallback", c_void_p)
    ]


class QnnInterface_t(Structure):
    _fields_ = [
        ("backendId",    c_uint32),
        ("providerName", c_char_p),
        ("apiVersion",   Qnn_ApiVersion_t),
        # NOTE: table follows immediately in memory → computed as (&apiVersion + sizeof)
    ]


# --- System provider structs ---
class QNN_SYSTEM_INTERFACE_VER_TYPE(Structure):
    _fields_ = [
        ("systemContextCreate",                    c_void_p),
        ("systemContextGetBinaryInfo",             c_void_p),
        ("systemContextGetMetaData",               c_void_p),
        ("systemContextFree",                      c_void_p),
        ("systemTensorGetMemoryFootprint",         c_void_p),
        ("systemLogCreate",                        c_void_p),
        ("systemLogSetLogLevel",                   c_void_p),
        ("systemLogFree",                          c_void_p),
        ("systemDlcCreateFromFile",                c_void_p),
        ("systemDlcCreateFromBinary",              c_void_p),
        ("systemDlcComposeGraphs",                 c_void_p),
        ("systemDlcGetOpMappings",                 c_void_p),
        ("systemDlcFree",                          c_void_p),
        ("systemProfileCreateSerializationTarget", c_void_p),
        ("systemProfileSerializeEventData",        c_void_p),
        ("systemProfileFreeSerializationTarget",   c_void_p),
    ]


class QnnSystemInterface_t(Structure):
    _fields_ = [
        ("backendId",        c_uint32),
        ("providerName",     c_char_p),
        ("systemApiVersion", Qnn_Version_t),
        # NOTE: table follows BY VALUE → computed as (addressof(struct) + sizeof(struct))
    ]


# =========================================================================================
# 2) Prototypes and Log Callback
# =========================================================================================

# --------------------------- Property / Global Config ------------------------------------
# typedef Qnn_ErrorHandle_t (*QnnProperty_HasCapabilityFn_t)(QnnProperty_Key_t key);
QnnProperty_HasCapabilityFn_t = CFUNCTYPE(c_int, c_uint32)

# typedef Qnn_ErrorHandle_t (*QnnGlobalConfig_SetFn_t)(const QnnGlobalConfig_t** config);
QnnGlobalConfig_SetFn_t = CFUNCTYPE(c_int, POINTER(POINTER(c_void_p)))

# --------------------------------------- Backend -----------------------------------------
# typedef Qnn_ErrorHandle_t (*QnnBackend_CreateFn_t)(
#   Qnn_LogHandle_t logger, const QnnBackend_Config_t** config, Qnn_BackendHandle_t* backend);
QnnBackend_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnBackend_SetConfigFn_t)(Qnn_BackendHandle_t backend, const QnnBackend_Config_t** config);
QnnBackend_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# typedef Qnn_ErrorHandle_t (*QnnBackend_GetApiVersionFn_t)(Qnn_ApiVersion_t* pVersion);
QnnBackend_GetApiVersionFn_t = CFUNCTYPE(c_int, POINTER(Qnn_ApiVersion_t))

# typedef Qnn_ErrorHandle_t (*QnnBackend_GetBuildIdFn_t)(const char** id);
QnnBackend_GetBuildIdFn_t = CFUNCTYPE(c_int, POINTER(
    c_char_p))

# typedef Qnn_ErrorHandle_t (*QnnBackend_RegisterOpPackageFn_t)(
#   Qnn_BackendHandle_t backend, const char* packagePath, const char* interfaceProvider, const char* target);
QnnBackend_RegisterOpPackageFn_t = CFUNCTYPE(
    c_int, c_void_p, c_char_p, c_char_p, c_char_p)

# typedef Qnn_ErrorHandle_t (*QnnBackend_GetSupportedOperationsFn_t)(
#   Qnn_BackendHandle_t backend, uint32_t* numOperations, const QnnBackend_OperationName_t** operations);
QnnBackend_GetSupportedOperationsFn_t = CFUNCTYPE(
    # ** to names
    c_int, c_void_p, POINTER(c_uint32), POINTER(POINTER(c_char_p)))

# typedef Qnn_ErrorHandle_t (*QnnBackend_ValidateOpConfigFn_t)(Qnn_BackendHandle_t backend, Qnn_OpConfig_t opConfig);
QnnBackend_ValidateOpConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnBackend_FreeFn_t)(Qnn_BackendHandle_t backend);
QnnBackend_FreeFn_t = CFUNCTYPE(c_int, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnBackend_GetPropertyFn_t)(Qnn_BackendHandle_t backendHandle, QnnBackend_Property_t** properties);
QnnBackend_GetPropertyFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# --------------------------------------- Context -----------------------------------------
# typedef Qnn_ErrorHandle_t (*QnnContext_CreateFn_t)(
#   Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t device, const QnnContext_Config_t** config, Qnn_ContextHandle_t* context);
QnnContext_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, POINTER(POINTER(c_void_p)), POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnContext_SetConfigFn_t)(Qnn_ContextHandle_t context, const QnnContext_Config_t** config);
QnnContext_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# typedef Qnn_ErrorHandle_t (*QnnContext_GetBinarySizeFn_t)(Qnn_ContextHandle_t context, Qnn_ContextBinarySize_t* binaryBufferSize);
QnnContext_GetBinarySizeFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(ctypes.c_size_t))

# typedef Qnn_ErrorHandle_t (*QnnContext_GetBinaryFn_t)(Qnn_ContextHandle_t context, void* binaryBuffer,
#   Qnn_ContextBinarySize_t binaryBufferSize, Qnn_ContextBinarySize_t* writtenBufferSize);
QnnContext_GetBinaryFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, ctypes.c_size_t, POINTER(ctypes.c_size_t))

# typedef Qnn_ErrorHandle_t (*QnnContext_CreateFromBinaryFn_t)(
#   Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t device, const QnnContext_Config_t** config,
#   const void* binaryBuffer, Qnn_ContextBinarySize_t binaryBufferSize, Qnn_ContextHandle_t* context, Qnn_ProfileHandle_t profile);
QnnContext_CreateFromBinaryFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, POINTER(
    POINTER(c_void_p)), c_void_p, ctypes.c_size_t, POINTER(c_void_p), c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_FreeFn_t)(Qnn_ContextHandle_t context, Qnn_ProfileHandle_t profile);
QnnContext_FreeFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_ValidateBinaryFn_t)(
#   Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t device, const QnnContext_Config_t** config,
#   const void* binaryBuffer, Qnn_ContextBinarySize_t binaryBufferSize);
QnnContext_ValidateBinaryFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, POINTER(
    POINTER(c_void_p)), c_void_p, ctypes.c_size_t)

# typedef Qnn_ErrorHandle_t (*QnnContext_CreateFromBinaryWithSignalFn_t)(
#   ..., const void* binaryBuffer, Qnn_ContextBinarySize_t binaryBufferSize, Qnn_ContextHandle_t* context,
#   Qnn_ProfileHandle_t profile, Qnn_SignalHandle_t signal);
QnnContext_CreateFromBinaryWithSignalFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, POINTER(
    POINTER(c_void_p)), c_void_p, ctypes.c_size_t, POINTER(c_void_p), c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_CreateFromBinaryListAsyncFn_t)(
#   Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t device, const QnnContext_Params_t** contextParams,
#   const QnnContext_Config_t** listConfig, Qnn_SignalHandle_t signal);
QnnContext_CreateFromBinaryListAsyncFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, POINTER(
    POINTER(c_void_p)), POINTER(POINTER(c_void_p)), c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_FinalizeFn_t)(Qnn_ContextHandle_t context, Qnn_ProfileHandle_t profile);
QnnContext_FinalizeFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_CreateFromBinaryWithCallbackFn_t)(
#   Qnn_BackendHandle_t backend, Qnn_DeviceHandle_t device, const QnnContext_Config_t** config,
#   const Qnn_ContextBinaryCallback_t* callback, const void* binaryBuffer, Qnn_ContextBinarySize_t binaryBufferSize,
#   Qnn_ContextHandle_t* context, Qnn_ProfileHandle_t profile, Qnn_SignalHandle_t signal);
QnnContext_CreateFromBinaryWithCallbackFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, POINTER(
    POINTER(c_void_p)), c_void_p, c_void_p, ctypes.c_size_t, POINTER(c_void_p), c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_GetBinarySectionSizeFn_t)(
#   Qnn_ContextHandle_t context, Qnn_GraphHandle_t graph, QnnContext_SectionType_t section, Qnn_ContextBinarySize_t* size);
QnnContext_GetBinarySectionSizeFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, c_uint32, POINTER(ctypes.c_size_t))

# typedef Qnn_ErrorHandle_t (*QnnContext_GetBinarySectionFn_t)(
#   Qnn_ContextHandle_t context, Qnn_GraphHandle_t graph, QnnContext_SectionType_t section,
#   const QnnContext_Buffer_t* binaryBuffer, Qnn_ContextBinarySize_t* written, Qnn_ProfileHandle_t profile, Qnn_SignalHandle_t signal);
QnnContext_GetBinarySectionFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, c_uint32, c_void_p, POINTER(ctypes.c_size_t), c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_ApplyBinarySectionFn_t)(
#   Qnn_ContextHandle_t context, Qnn_GraphHandle_t graph, QnnContext_SectionType_t section,
#   const QnnContext_Buffer_t* binaryBuffer, Qnn_ProfileHandle_t profile, Qnn_SignalHandle_t signal);
QnnContext_ApplyBinarySectionFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, c_uint32, c_void_p, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnContext_GetPropertyFn_t)(Qnn_ContextHandle_t contextHandle, QnnContext_Property_t** properties);
QnnContext_GetPropertyFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# typedef Qnn_ErrorHandle_t (*QnnContext_GetIncrementalBinaryFn_t)(
#   Qnn_ContextHandle_t context, const void** binaryBuffer, Qnn_ContextBinarySize_t* startOffset, Qnn_ContextBinarySize_t* written);
QnnContext_GetIncrementalBinaryFn_t = CFUNCTYPE(c_int, c_void_p, POINTER(
    # const void**
    c_void_p), POINTER(ctypes.c_size_t), POINTER(ctypes.c_size_t))

# typedef Qnn_ErrorHandle_t (*QnnContext_ReleaseIncrementalBinaryFn_t)(
#   Qnn_ContextHandle_t context, const void* binaryBuffer, Qnn_ContextBinarySize_t startOffset);
QnnContext_ReleaseIncrementalBinaryFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p, ctypes.c_size_t)

# ---------------------------------------- Graph ------------------------------------------
# typedef Qnn_ErrorHandle_t (*QnnGraph_CreateFn_t)(
#   Qnn_ContextHandle_t contextHandle, const char* graphName, const QnnGraph_Config_t** config, Qnn_GraphHandle_t* graphHandle);
QnnGraph_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, c_char_p, POINTER(POINTER(c_void_p)), POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnGraph_CreateSubgraphFn_t)(
#   Qnn_GraphHandle_t graphHandle, const char* graphName, Qnn_GraphHandle_t* subgraphHandle);
QnnGraph_CreateSubgraphFn_t = CFUNCTYPE(
    c_int, c_void_p, c_char_p, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnGraph_SetConfigFn_t)(Qnn_GraphHandle_t graphHandle, const QnnGraph_Config_t** config);
QnnGraph_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# typedef Qnn_ErrorHandle_t (*QnnGraph_GetPropertyFn_t)(Qnn_GraphHandle_t graphHandle, QnnGraph_Property_t** properties);
QnnGraph_GetPropertyFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))

# typedef Qnn_ErrorHandle_t (*QnnGraph_AddNodeFn_t)(Qnn_GraphHandle_t graphHandle, Qnn_OpConfig_t opConfig);
# opConfig by value (opaque)
QnnGraph_AddNodeFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnGraph_FinalizeFn_t)(
#   Qnn_GraphHandle_t graphHandle, Qnn_ProfileHandle_t profileHandle, Qnn_SignalHandle_t signalHandle);
QnnGraph_FinalizeFn_t = CFUNCTYPE(c_int, c_void_p, c_void_p, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnGraph_RetrieveFn_t)(
#   Qnn_ContextHandle_t contextHandle, const char* graphName, Qnn_GraphHandle_t* graphHandle);
QnnGraph_RetrieveFn_t = CFUNCTYPE(c_int, c_void_p, c_char_p, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnGraph_PrepareExecutionEnvironmentFn_t)(
#   Qnn_GraphHandle_t graphHandle, QnnGraph_ExecuteEnvironment_t** envs, uint32_t envSize);
QnnGraph_PrepareExecutionEnvironmentFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), c_uint32)

# typedef Qnn_ErrorHandle_t (*QnnGraph_ExecuteFn_t)(
#   Qnn_GraphHandle_t graphHandle, const Qnn_Tensor_t* inputs, uint32_t numInputs,
#   Qnn_Tensor_t* outputs, uint32_t numOutputs, Qnn_ProfileHandle_t profileHandle, Qnn_SignalHandle_t signalHandle);
QnnGraph_ExecuteFn_t = CFUNCTYPE(
    c_int,
    c_void_p,
    POINTER(Qnn_Tensor_t), c_uint32,
    POINTER(Qnn_Tensor_t), c_uint32,
    c_void_p, c_void_p
)

# typedef Qnn_ErrorHandle_t (*QnnGraph_ExecuteAsyncFn_t)(
#   ..., Qnn_NotifyFn_t notifyFn, void* notifyParam);
QnnGraph_ExecuteAsyncFn_t = CFUNCTYPE(
    c_int,
    c_void_p,
    POINTER(Qnn_Tensor_t), c_uint32,
    POINTER(Qnn_Tensor_t), c_uint32,
    c_void_p, c_void_p,
    c_void_p, c_void_p
)

# typedef Qnn_ErrorHandle_t (*QnnGraph_ReleaseExecutionEnvironmentFn_t)(
#   Qnn_GraphHandle_t graphHandle, const QnnGraph_ExecuteEnvironment_t** envs, uint32_t envSize);
QnnGraph_ReleaseExecutionEnvironmentFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), c_uint32)

# ---------------------------------------- Tensor -----------------------------------------
# typedef Qnn_ErrorHandle_t (*QnnTensor_CreateContextTensorFn_t)(Qnn_ContextHandle_t context, Qnn_Tensor_t* tensor);
QnnTensor_CreateContextTensorFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnTensor_CreateGraphTensorFn_t)(Qnn_GraphHandle_t graph, Qnn_Tensor_t* tensor);
QnnTensor_CreateGraphTensorFn_t = CFUNCTYPE(c_int, c_void_p, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnTensor_UpdateContextTensorsFn_t)(
#   Qnn_ContextHandle_t context, const Qnn_Tensor_t** tensor, uint64_t numTensors);
QnnTensor_UpdateContextTensorsFn_t = CFUNCTYPE(c_int, c_void_p, POINTER(
    POINTER(c_void_p)), ctypes.c_uint64)

# typedef Qnn_ErrorHandle_t (*QnnTensor_UpdateGraphTensorsFn_t)(
#   Qnn_GraphHandle_t graph, const Qnn_Tensor_t** tensor, uint64_t numTensors);
QnnTensor_UpdateGraphTensorsFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), ctypes.c_uint64)

# ----------------------------------------- Log -------------------------------------------
QnnLog_Callback_t = CFUNCTYPE(
    None, c_char_p, c_uint32, ctypes.c_uint64, c_void_p)

# typedef Qnn_ErrorHandle_t (*QnnLog_CreateFn_t)(
#   QnnLog_Callback_t callback, QnnLog_Level_t maxLogLevel, Qnn_LogHandle_t* logger);
QnnLog_CreateFn_t = CFUNCTYPE(
    c_int, QnnLog_Callback_t, ctypes.c_int, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnLog_SetLogLevelFn_t)(Qnn_LogHandle_t logger, QnnLog_Level_t maxLogLevel);
QnnLog_SetLogLevelFn_t = CFUNCTYPE(c_int, c_void_p, ctypes.c_int)

# typedef Qnn_ErrorHandle_t (*QnnLog_FreeFn_t)(Qnn_LogHandle_t logger);
QnnLog_FreeFn_t = CFUNCTYPE(c_int, c_void_p)

# --------------------------------------- Profile -----------------------------------------
QnnProfile_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, ctypes.c_int, POINTER(c_void_p))
QnnProfile_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))
QnnProfile_GetEventsFn_t = CFUNCTYPE(c_int, c_void_p, POINTER(
    POINTER(ctypes.c_uint64)), POINTER(ctypes.c_uint32))
QnnProfile_GetSubEventsFn_t = CFUNCTYPE(c_int, ctypes.c_uint64, POINTER(
    POINTER(ctypes.c_uint64)), POINTER(ctypes.c_uint32))
QnnProfile_GetEventDataFn_t = CFUNCTYPE(c_int, ctypes.c_uint64, c_void_p)
QnnProfile_GetExtendedEventDataFn_t = CFUNCTYPE(
    c_int, ctypes.c_uint64, c_void_p)
QnnProfile_FreeFn_t = CFUNCTYPE(c_int, c_void_p)

# ---------------------------------------- Memory -----------------------------------------
QnnMem_RegisterFn_t = CFUNCTYPE(
    # memDescriptors*
    c_int, c_void_p, c_void_p, c_uint32, POINTER(c_void_p))
QnnMem_DeRegisterFn_t = CFUNCTYPE(c_int, POINTER(
    c_void_p), c_uint32)

# ---------------------------------------- Device -----------------------------------------
QnnDevice_GetPlatformInfoFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))
QnnDevice_FreePlatformInfoFn_t = CFUNCTYPE(
    c_int, c_void_p, c_void_p)
QnnDevice_GetInfrastructureFn_t = CFUNCTYPE(
    c_int, c_void_p)
QnnDevice_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), POINTER(c_void_p))
QnnDevice_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))
QnnDevice_GetInfoFn_t = CFUNCTYPE(c_int, c_void_p, POINTER(
    POINTER(c_void_p)))
QnnDevice_FreeFn_t = CFUNCTYPE(c_int, c_void_p)

# ---------------------------------------- Signal -----------------------------------------
QnnSignal_CreateFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)), POINTER(c_void_p))
QnnSignal_SetConfigFn_t = CFUNCTYPE(
    c_int, c_void_p, POINTER(POINTER(c_void_p)))
QnnSignal_TriggerFn_t = CFUNCTYPE(c_int, c_void_p)
QnnSignal_FreeFn_t = CFUNCTYPE(c_int, c_void_p)

# -------------------------------------- Error helpers ------------------------------------
QnnError_GetMessageFn_t = CFUNCTYPE(c_int, c_int, POINTER(c_char_p))
QnnError_GetVerboseMessageFn_t = CFUNCTYPE(c_int, c_int, POINTER(c_char_p))
QnnError_FreeVerboseMessageFn_t = CFUNCTYPE(c_int, c_char_p)

# ----------------------------- System Context -----------------------------

# typedef Qnn_ErrorHandle_t (*QnnSystemContext_CreateFn_t)(QnnSystemContext_Handle_t* sysCtxHandle);
QnnSystemContext_CreateFn_t = CFUNCTYPE(ctypes.c_int, POINTER(c_void_p))

# int QnnSystemContext_getBinaryInfo(QnnSystemContext_Handle_t, void*, uint64_t,
#                                    const QnnSystemContext_BinaryInfo_t**, Qnn_ContextBinarySize_t*)
QnnSystemContext_GetBinaryInfoFn_t = CFUNCTYPE(
    c_int,
    c_void_p,                    # sysCtxHandle
    c_void_p,                    # binaryBuffer
    c_uint64,                    # binaryBufferSize (uint64_t)
    POINTER(POINTER(QnnSystemContext_BinaryInfo_t)),  # out: const BinaryInfo_t**
    POINTER(c_size_t),           # out: size_t* binaryInfoSize
)

# int QnnSystemContext_getMetadata(QnnSystemContext_Handle_t, const void*, Qnn_ContextBinarySize_t,
#                                  const QnnSystemContext_BinaryInfo_t**)
QnnSystemContext_GetMetaDataFn_t = CFUNCTYPE(
    c_int,
    c_void_p,                    # sysCtxHandle
    c_void_p,                    # binaryBuffer (const void*)
    c_size_t,                    # binaryBufferSize (Qnn_ContextBinarySize_t == size_t)
    POINTER(POINTER(QnnSystemContext_BinaryInfo_t)),  # out: const BinaryInfo_t**
)

# typedef Qnn_ErrorHandle_t (*QnnSystemContext_FreeFn_t)(QnnSystemContext_Handle_t sysCtxHandle);
QnnSystemContext_FreeFn_t = CFUNCTYPE(ctypes.c_int, c_void_p)

# ----------------------------- System Tensor ------------------------------

# typedef Qnn_ErrorHandle_t (*QnnSystemTensor_getMemoryFootprintFn_t)(Qnn_Tensor_t tensor, uint64_t* footprint);
QnnSystemTensor_getMemoryFootprintFn_t = CFUNCTYPE(
    ctypes.c_int, Qnn_Tensor_t, POINTER(c_uint64))

# ------------------------------ System Log --------------------------------

# typedef Qnn_ErrorHandle_t (*QnnSystemLog_createFn_t)(QnnLog_Callback_t callback, QnnLog_Level_t maxLogLevel, Qnn_LogHandle_t* logger);
QnnSystemLog_createFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p, ctypes.c_int, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnSystemLog_setLogLevelFn_t)(Qnn_LogHandle_t logger, QnnLog_Level_t maxLogLevel);
QnnSystemLog_setLogLevelFn_t = CFUNCTYPE(ctypes.c_int, c_void_p, ctypes.c_int)

# typedef Qnn_ErrorHandle_t (*QnnSystemLog_freeFn_t)(Qnn_LogHandle_t logger);
QnnSystemLog_freeFn_t = CFUNCTYPE(ctypes.c_int, c_void_p)

# ------------------------------ System DLC --------------------------------

# typedef Qnn_ErrorHandle_t (*QnnSystemDlc_createFromFileFn_t)(Qnn_LogHandle_t logger, const char* dlcPath, QnnSystemDlc_Handle_t* dlcHandle);
QnnSystemDlc_createFromFileFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p, c_char_p, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnSystemDlc_createFromBinaryFn_t)(Qnn_LogHandle_t logger, const uint8_t* buffer, const Qnn_ContextBinarySize_t bufferSize, QnnSystemDlc_Handle_t* dlcHandle);
QnnSystemDlc_createFromBinaryFn_t = CFUNCTYPE(ctypes.c_int, c_void_p, POINTER(
    ctypes.c_uint8), ctypes.c_size_t, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnSystemDlc_composeGraphsFn_t)(
#   QnnSystemDlc_Handle_t dlcHandle,
#   const QnnSystemDlc_GraphConfigInfo_t** graphConfigs,
#   const uint32_t numGraphConfigs,
#   Qnn_BackendHandle_t backend,
#   Qnn_ContextHandle_t context,
#   QnnInterface_t interface,
#   QnnSystemContext_GraphInfoVersion_t graphVersion,
#   QnnSystemContext_GraphInfo_t** graphs,
#   uint32_t* numGraphs);
QnnSystemDlc_composeGraphsFn_t = CFUNCTYPE(
    ctypes.c_int,
    c_void_p,                      # dlcHandle
    POINTER(POINTER(c_void_p)),    # graphConfigs (const T**)
    c_uint32,                      # numGraphConfigs
    c_void_p,                      # backend
    c_void_p,                      # context
    c_void_p,                      # interface (opaque QnnInterface_t)
    c_uint32,                      # graphVersion (enum/uint32)
    POINTER(POINTER(c_void_p)),    # graphs (T** out)
    POINTER(c_uint32)              # numGraphs (uint32_t*)
)

# typedef Qnn_ErrorHandle_t (*QnnSystemDlc_getOpMappingsFn_t)(QnnSystemDlc_Handle_t dlcHandle, const Qnn_OpMapping_t** opMappings, uint32_t* numOpMappings);
QnnSystemDlc_getOpMappingsFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p, POINTER(POINTER(c_void_p)), POINTER(c_uint32))

# typedef Qnn_ErrorHandle_t (*QnnSystemDlc_freeFn_t)(QnnSystemDlc_Handle_t dlcHandle);
QnnSystemDlc_freeFn_t = CFUNCTYPE(ctypes.c_int, c_void_p)

# --------------------------- System Profile --------------------------------

# typedef Qnn_ErrorHandle_t (*QnnSystemProfile_createSerializationTargetFn_t)(
#   QnnSystemProfile_SerializationTarget_t serializationTargetInfo,
#   QnnSystemProfile_SerializationTargetConfig_t* configs,
#   uint32_t numConfigs,
#   QnnSystemProfile_SerializationTargetHandle_t* serializationTarget);
QnnSystemProfile_createSerializationTargetFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p, c_void_p, c_uint32, POINTER(c_void_p))

# typedef Qnn_ErrorHandle_t (*QnnSystemProfile_serializeEventDataFn_t)(
#   QnnSystemProfile_SerializationTargetHandle_t serializationTarget,
#   const QnnSystemProfile_ProfileData_t** eventData,
#   uint32_t numEvents);
QnnSystemProfile_serializeEventDataFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p, POINTER(POINTER(c_void_p)), c_uint32)

# typedef Qnn_ErrorHandle_t (*QnnSystemProfile_freeSerializationTargetFn_t)(QnnSystemProfile_SerializationTargetHandle_t serializationTarget);
QnnSystemProfile_freeSerializationTargetFn_t = CFUNCTYPE(
    ctypes.c_int, c_void_p)


_LIBC = ctypes.CDLL(None)
_LIBC_VSNPRINTF = _LIBC.vsnprintf
_LIBC_VSNPRINTF.restype = c_int
_LIBC_VSNPRINTF.argtypes = [c_char_p, c_size_t, c_char_p, c_void_p]


def _decode_log_format(fmt: c_char_p) -> str:
    if not fmt:
        return ""
    try:
        return ctypes.cast(fmt, c_char_p).value.decode(errors="replace")
    except Exception:
        return "<fmt decode error>"


def _format_log_message(fmt: c_char_p, va_list_ptr: c_void_p) -> str:
    raw_format = _decode_log_format(fmt)
    if not raw_format:
        return raw_format

    va_addr = ctypes.cast(va_list_ptr, c_void_p).value if va_list_ptr else None
    if not va_addr:
        return raw_format

    try:
        fmt_bytes = raw_format.encode("utf-8", errors="replace")
    except Exception:
        return raw_format

    try:
        # A va_list should not be reused after formatting, so use one sufficiently
        # large buffer and make a single vsnprintf call.
        buffer = ctypes.create_string_buffer(8192)
        written = _LIBC_VSNPRINTF(buffer, len(buffer), fmt_bytes, va_list_ptr)
        if written < 0:
            return raw_format

        message = buffer.value.decode("utf-8", errors="replace")
        if written >= len(buffer):
            return f"{message}... [truncated]"
        return message
    except Exception:
        return raw_format


@QnnLog_Callback_t
def CORE_LOG_CB(fmt: c_char_p, level: int, timestamp: int, va_list_ptr: c_void_p):
    s = _format_log_message(fmt, va_list_ptr)
    # Level mapping: 0=ERROR,1=WARN,2=INFO,3=DEBUG,4=VERBOSE (typical)
    level_names = {
        0: "ERROR",
        1: "WARN",
        2: "INFO",
        3: "DEBUG",
        4: "VERBOSE"
    }

    print(f"[CORE LOG] {level_names.get(level, level)} {timestamp} : {s}")


# =========================================================================================
# 3) Utilities for binding & error decoding
# =========================================================================================

class QnnErrHandling(Structure):
    _fields_ = [
        ("errorGetMessage",        QnnError_GetMessageFn_t),
        ("errorGetVerboseMessage", QnnError_GetVerboseMessageFn_t),
        ("errorFreeVerboseMessage", QnnError_FreeVerboseMessageFn_t),
    ]

class Binder:
    @staticmethod
    def callable(addr: int, proto, name: str):
        if not addr or addr < 0x1000:
            raise RuntimeError(f"{name} invalid pointer: 0x{addr:016x}")
        return ctypes.cast(addr, proto)


class ErrorHelper:
    @staticmethod
    def describe(err: QnnErrHandling, rc: int):
        sm, vm = None, None
        try:
            out = c_char_p()
            _ = err.errorGetMessage(int(rc), ctypes.byref(out))

            if out.value:
                sm = out.value.decode(errors="replace")
        except Exception as e:
            sm = f"(errorGetMessage failed: {e})"

        try:
            outv = c_char_p()
            _ = err.errorGetVerboseMessage(int(rc), ctypes.byref(outv))

            if outv.value:
                vm = outv.value.decode(errors="replace")

                try:
                    err.errorFreeVerboseMessage(outv.value)
                except Exception:
                    pass

        except Exception as e:
            vm = f"(errorGetVerboseMessage failed: {e})"

        return sm, vm

    @staticmethod
    def print(where: str, rc: int, sm: str, vm: str):
        parts = [f"[ERR] {where}: rc={rc}"]

        if sm:
            parts.append(f"| {sm}")

        if vm:
            parts.append(f"| {vm}")

        print(" ".join(parts))


# =========================================================================================
# 4) Library loader & provider access
# =========================================================================================
class QnnLibrary:
    def __init__(self, backend_name: str = "libQnnHtp.so", system_name: str = "libQnnSystem.so"):
        self.backend_name = backend_name
        self.system_name = system_name
        self.backend = None
        self.system = None

    def load(self):
        try:
            self.backend = ctypes.CDLL(
                self.backend_name, mode=ctypes.RTLD_GLOBAL)
        except OSError as e:
            raise RuntimeError(
                f"Failed to load backend '{self.backend_name}': {e}")
        try:
            self.system = ctypes.CDLL(
                self.system_name, mode=ctypes.RTLD_GLOBAL)
        except OSError as e:
            print(f"[WARN] Failed to load system '{self.system_name}': {e}")
            self.system = None

    def get_qnn_providers(self):
        fn = getattr(self.backend, "QnnInterface_getProviders", None)

        if not fn:
            raise RuntimeError(
                "QnnInterface_getProviders not exported by backend lib.")

        fn.restype = ctypes.c_int
        fn.argtypes = (POINTER(POINTER(POINTER(QnnInterface_t))),
                       POINTER(ctypes.c_uint32))
        pp = POINTER(POINTER(QnnInterface_t))()
        n = ctypes.c_uint32(0)
        rc = fn(ctypes.byref(pp), ctypes.byref(n))
        if rc != 0 or n.value == 0 or not pp:
            raise RuntimeError(f"getProviders failed rc={rc} count={n.value}")

        return pp, n.value

    def get_system_providers(self):
        if self.system is None:
            raise RuntimeError("System library not loaded.")

        fn = getattr(self.system, "QnnSystemInterface_getProviders", None)
        if not fn:
            raise RuntimeError(
                "QnnSystemInterface_getProviders not exported by system lib.")

        fn.restype = ctypes.c_int
        fn.argtypes = (
            POINTER(POINTER(POINTER(QnnSystemInterface_t))), POINTER(ctypes.c_uint32))
        pp = POINTER(POINTER(QnnSystemInterface_t))()
        n = ctypes.c_uint32(0)
        rc = fn(ctypes.byref(pp), ctypes.byref(n))

        if rc != 0 or n.value == 0 or not pp:
            raise RuntimeError(
                f"System getProviders failed rc={rc} count={n.value}")

        return pp, n.value


# =========================================================================================
# 5) Provider & Interface wrappers (Core + System)
# =========================================================================================
class QnnProvider:
    def __init__(self, provider_ptr: POINTER(QnnInterface_t)):
        self._p = provider_ptr.contents
        self._iface = None

        self.make_callable()

    def info(self):
        name = self._p.providerName.decode() if self._p.providerName else "(null)"
        cv = self._p.apiVersion.coreApiVersion
        bv = self._p.apiVersion.backendApiVersion

        return {
            "addr":         ctypes.addressof(self._p),
            "name_ptr":     (ctypes.cast(self._p.providerName, c_void_p).value or 0),
            "coreApi":      f"{int(cv.major)}.{int(cv.minor)}.{int(cv.patch)}",
            "backendApi":   f"{int(bv.major)}.{int(bv.minor)}.{int(bv.patch)}",
            "backendId":    int(self._p.backendId),
            "providerName": name,
        }

    def __interface(self) -> QNN_INTERFACE_VER_TYPE:
        if self._iface is None:
            base = ctypes.addressof(self._p.apiVersion)
            iface_addr = base + ctypes.sizeof(Qnn_ApiVersion_t)  # (p_api + 1)
            iface_ptr = ctypes.cast(
                c_void_p(iface_addr), POINTER(QNN_INTERFACE_VER_TYPE))
            self._iface = iface_ptr.contents

        return self._iface

    def compose_graphs(
        self,
        backend_handle: ctypes.c_void_p,
        context_handle: ctypes.c_void_p,
        model_interop,  # instance of QnnModelInterop
        debug: bool,
        log_level: int,
        graph_configs_pp: ctypes.POINTER(
            ctypes.POINTER(ctypes.c_void_p)) = None,
        num_graph_configs: int = 0
    ):
        # Ensure callable function table is available
        iface = self.__interface()

        # Defaults for configs if none are provided
        if graph_configs_pp is None:
            graph_configs_pp = ctypes.POINTER(
                ctypes.POINTER(ctypes.c_void_p))()
            num_graph_configs = 0

        # Outputs
        graphs_ppp = ctypes.POINTER(
            ctypes.POINTER(ctypes.POINTER(GraphInfo)))()
        num_graphs = ctypes.c_uint32(0)

        # Call into the model’s composeGraphs with the typed interface table
        rc = model_interop.compose(
            backend_handle,
            iface,
            context_handle,
            graph_configs_pp,
            ctypes.c_uint32(num_graph_configs),
            ctypes.byref(graphs_ppp),
            ctypes.byref(num_graphs),
            ctypes.c_bool(bool(debug)),
            CORE_LOG_CB,
            ctypes.c_int(int(log_level)),
        )

        if rc != 0:
            raise RuntimeError(f"composeGraphs failed rc={rc}")

        return graphs_ppp, int(num_graphs.value)

    def make_callable(self):
        self.__iface = self.__interface()

        # --------------------------- Property / Global Config ------------------------------------
        self.propertyHasCapability = Binder.callable(
            self.__iface.propertyHasCapability,
            QnnProperty_HasCapabilityFn_t,
            "propertyHasCapability")
        self.globalConfigSet = Binder.callable(
            self.__iface.globalConfigSet,
            QnnGlobalConfig_SetFn_t,
            "globalConfigSet")

        # --------------------------------------- Backend -----------------------------------------
        self.backendCreate = Binder.callable(
            self.__iface.backendCreate,
            QnnBackend_CreateFn_t,
            "backendCreate")
        self.backendSetConfig = Binder.callable(
            self.__iface.backendSetConfig,
            QnnBackend_SetConfigFn_t,
            "backendSetConfig")
        self.backendGetApiVersion = Binder.callable(
            self.__iface.backendGetApiVersion,
            QnnBackend_GetApiVersionFn_t,
            "backendGetApiVersion")
        self.backendGetBuildId = Binder.callable(
            self.__iface.backendGetBuildId,
            QnnBackend_GetBuildIdFn_t,
            "backendGetBuildId")
        self.backendRegisterOpPackage = Binder.callable(
            self.__iface.backendRegisterOpPackage,
            QnnBackend_RegisterOpPackageFn_t,
            "backendRegisterOpPackage")
        self.backendGetSupportedOperations = Binder.callable(
            self.__iface.backendGetSupportedOperations,
            QnnBackend_GetSupportedOperationsFn_t,
            "backendGetSupportedOperations")
        self.backendValidateOpConfig = Binder.callable(
            self.__iface.backendValidateOpConfig,
            QnnBackend_ValidateOpConfigFn_t,
            "backendValidateOpConfig")
        self.backendFree = Binder.callable(
            self.__iface.backendFree,
            QnnBackend_FreeFn_t,
            "backendFree")
        self.backendGetProperty = Binder.callable(
            self.__iface.backendGetProperty,
            QnnBackend_GetPropertyFn_t,
            "backendGetProperty")

        # --------------------------------------- Context -----------------------------------------
        self.contextCreate = Binder.callable(
            self.__iface.contextCreate,
            QnnContext_CreateFn_t,
            "contextCreate")
        self.contextSetConfig = Binder.callable(
            self.__iface.contextSetConfig,
            QnnContext_SetConfigFn_t,
            "contextSetConfig")
        self.contextGetBinarySize = Binder.callable(
            self.__iface.contextGetBinarySize,
            QnnContext_GetBinarySizeFn_t,
            "contextGetBinarySize")
        self.contextGetBinary = Binder.callable(
            self.__iface.contextGetBinary,
            QnnContext_GetBinaryFn_t,
            "contextGetBinary")
        self.contextCreateFromBinary = Binder.callable(
            self.__iface.contextCreateFromBinary,
            QnnContext_CreateFromBinaryFn_t,
            "contextCreateFromBinary")
        self.contextFree = Binder.callable(
            self.__iface.contextFree,
            QnnContext_FreeFn_t,
            "contextFree")
        self.contextValidateBinary = Binder.callable(
            self.__iface.contextValidateBinary,
            QnnContext_ValidateBinaryFn_t,
            "contextValidateBinary")
        self.contextCreateFromBinaryWithSignal = Binder.callable(
            self.__iface.contextCreateFromBinaryWithSignal,
            QnnContext_CreateFromBinaryWithSignalFn_t,
            "contextCreateFromBinaryWithSignal")
        self.contextCreateFromBinaryListAsync = Binder.callable(
            self.__iface.contextCreateFromBinaryListAsync,
            QnnContext_CreateFromBinaryListAsyncFn_t,
            "contextCreateFromBinaryListAsync")
        self.contextFinalize = Binder.callable(
            self.__iface.contextFinalize,
            QnnContext_FinalizeFn_t,
            "contextFinalize")
        self.contextCreateFromBinaryWithCallback = Binder.callable(
            self.__iface.contextCreateFromBinaryWithCallback,
            QnnContext_CreateFromBinaryWithCallbackFn_t,
            "contextCreateFromBinaryWithCallback")
        self.contextGetBinarySectionSize = Binder.callable(
            self.__iface.contextGetBinarySectionSize,
            QnnContext_GetBinarySectionSizeFn_t,
            "contextGetBinarySectionSize")
        self.contextGetBinarySection = Binder.callable(
            self.__iface.contextGetBinarySection,
            QnnContext_GetBinarySectionFn_t,
            "contextGetBinarySection")
        self.contextApplyBinarySection = Binder.callable(
            self.__iface.contextApplyBinarySection,
            QnnContext_ApplyBinarySectionFn_t,
            "contextApplyBinarySection")
        self.contextGetProperty = Binder.callable(
            self.__iface.contextGetProperty,
            QnnContext_GetPropertyFn_t,
            "contextGetProperty")
        self.contextGetIncrementalBinary = Binder.callable(
            self.__iface.contextGetIncrementalBinary,
            QnnContext_GetIncrementalBinaryFn_t,
            "contextGetIncrementalBinary")
        self.contextReleaseIncrementalBinary = Binder.callable(
            self.__iface.contextReleaseIncrementalBinary,
            QnnContext_ReleaseIncrementalBinaryFn_t,
            "contextReleaseIncrementalBinary")

        # ---------------------------------------- Graph ------------------------------------------
        self.graphCreate = Binder.callable(
            self.__iface.graphCreate,
            QnnGraph_CreateFn_t,
            "graphCreate")
        self.graphCreateSubgraph = Binder.callable(
            self.__iface.graphCreateSubgraph,
            QnnGraph_CreateSubgraphFn_t,
            "graphCreateSubgraph")
        self.graphSetConfig = Binder.callable(
            self.__iface.graphSetConfig,
            QnnGraph_SetConfigFn_t,
            "graphSetConfig")
        self.graphGetProperty = Binder.callable(
            self.__iface.graphGetProperty,
            QnnGraph_GetPropertyFn_t,
            "graphGetProperty")
        self.graphAddNode = Binder.callable(
            self.__iface.graphAddNode,
            QnnGraph_AddNodeFn_t,
            "graphAddNode")
        self.graphFinalize = Binder.callable(
            self.__iface.graphFinalize,
            QnnGraph_FinalizeFn_t,
            "graphFinalize")
        self.graphRetrieve = Binder.callable(
            self.__iface.graphRetrieve,
            QnnGraph_RetrieveFn_t,
            "graphRetrieve")
        self.graphPrepareExecutionEnvironment = Binder.callable(
            self.__iface.graphPrepareExecutionEnvironment,
            QnnGraph_PrepareExecutionEnvironmentFn_t,
            "graphPrepareExecutionEnvironment")
        self.graphExecute = Binder.callable(
            self.__iface.graphExecute,
            QnnGraph_ExecuteFn_t,
            "graphExecute")
        self.graphExecuteAsync = Binder.callable(
            self.__iface.graphExecuteAsync,
            QnnGraph_ExecuteAsyncFn_t,
            "graphExecuteAsync")
        self.graphReleaseExecutionEnvironment = Binder.callable(
            self.__iface.graphReleaseExecutionEnvironment,
            QnnGraph_ReleaseExecutionEnvironmentFn_t,
            "graphReleaseExecutionEnvironment")

        # ---------------------------------------- Tensor -----------------------------------------
        self.tensorCreateContextTensor = Binder.callable(
            self.__iface.tensorCreateContextTensor,
            QnnTensor_CreateContextTensorFn_t,
            "tensorCreateContextTensor")
        self.tensorCreateGraphTensor = Binder.callable(
            self.__iface.tensorCreateGraphTensor,
            QnnTensor_CreateGraphTensorFn_t,
            "tensorCreateGraphTensor")
        self.tensorUpdateContextTensors = Binder.callable(
            self.__iface.tensorUpdateContextTensors,
            QnnTensor_UpdateContextTensorsFn_t,
            "tensorUpdateContextTensors")
        self.tensorUpdateGraphTensors = Binder.callable(
            self.__iface.tensorUpdateGraphTensors,
            QnnTensor_UpdateGraphTensorsFn_t,
            "tensorUpdateGraphTensors")

        # ----------------------------------------- Log -------------------------------------------
        self.logCreate = Binder.callable(
            self.__iface.logCreate,
            QnnLog_CreateFn_t,
            "logCreate")
        self.logSetLogLevel = Binder.callable(
            self.__iface.logSetLogLevel,
            QnnLog_SetLogLevelFn_t,
            "logSetLogLevel")
        self.logFree = Binder.callable(
            self.__iface.logFree,
            QnnLog_FreeFn_t,
            "logFree")

        # --------------------------------------- Profile -----------------------------------------
        self.profileCreate = Binder.callable(
            self.__iface.profileCreate,
            QnnProfile_CreateFn_t,
            "profileCreate")
        self.profileSetConfig = Binder.callable(
            self.__iface.profileSetConfig,
            QnnProfile_SetConfigFn_t,
            "profileSetConfig")
        self.profileGetEvents = Binder.callable(
            self.__iface.profileGetEvents,
            QnnProfile_GetEventsFn_t,
            "profileGetEvents")
        self.profileGetSubEvents = Binder.callable(
            self.__iface.profileGetSubEvents,
            QnnProfile_GetSubEventsFn_t,
            "profileGetSubEvents")
        self.profileGetEventData = Binder.callable(
            self.__iface.profileGetEventData,
            QnnProfile_GetEventDataFn_t,
            "profileGetEventData")
        self.profileGetExtendedEventData = Binder.callable(
            self.__iface.profileGetExtendedEventData,
            QnnProfile_GetExtendedEventDataFn_t,
            "profileGetExtendedEventData")
        self.profileFree = Binder.callable(
            self.__iface.profileFree,
            QnnProfile_FreeFn_t,
            "profileFree")

        # ---------------------------------------- Memory -----------------------------------------
        self.memRegister = Binder.callable(
            self.__iface.memRegister,
            QnnMem_RegisterFn_t,
            "memRegister")
        self.memDeRegister = Binder.callable(
            self.__iface.memDeRegister,
            QnnMem_DeRegisterFn_t,
            "memDeRegister")

        # ---------------------------------------- Device -----------------------------------------
        self.deviceGetPlatformInfo = Binder.callable(
            self.__iface.deviceGetPlatformInfo,
            QnnDevice_GetPlatformInfoFn_t,
            "deviceGetPlatformInfo")
        self.deviceFreePlatformInfo = Binder.callable(
            self.__iface.deviceFreePlatformInfo,
            QnnDevice_FreePlatformInfoFn_t,
            "deviceFreePlatformInfo")
        self.deviceGetInfrastructure = Binder.callable(
            self.__iface.deviceGetInfrastructure,
            QnnDevice_GetInfrastructureFn_t,
            "deviceGetInfrastructure")
        self.deviceCreate = Binder.callable(
            self.__iface.deviceCreate,
            QnnDevice_CreateFn_t,
            "deviceCreate")
        self.deviceSetConfig = Binder.callable(
            self.__iface.deviceSetConfig,
            QnnDevice_SetConfigFn_t,
            "deviceSetConfig")
        self.deviceGetInfo = Binder.callable(
            self.__iface.deviceGetInfo,
            QnnDevice_GetInfoFn_t,
            "deviceGetInfo")
        self.deviceFree = Binder.callable(
            self.__iface.deviceFree,
            QnnDevice_FreeFn_t,
            "deviceFree")

        # ---------------------------------------- Signal -----------------------------------------
        self.signalCreate = Binder.callable(
            self.__iface.signalCreate,
            QnnSignal_CreateFn_t,
            "signalCreate")
        self.signalSetConfig = Binder.callable(
            self.__iface.signalSetConfig,
            QnnSignal_SetConfigFn_t,
            "signalSetConfig")
        self.signalTrigger = Binder.callable(
            self.__iface.signalTrigger,
            QnnSignal_TriggerFn_t,
            "signalTrigger")
        self.signalFree = Binder.callable(
            self.__iface.signalFree,
            QnnSignal_FreeFn_t,
            "signalFree")

        # -------------------------------------- Error helpers ------------------------------------
        errorGetMessage = Binder.callable(
            self.__iface.errorGetMessage,
            QnnError_GetMessageFn_t,
            "errorGetMessage")
        errorGetVerboseMessage = Binder.callable(
            self.__iface.errorGetVerboseMessage,
            QnnError_GetVerboseMessageFn_t,
            "errorGetVerboseMessage")
        errorFreeVerboseMessage = Binder.callable(
            self.__iface.errorFreeVerboseMessage,
            QnnError_FreeVerboseMessageFn_t,
            "errorFreeVerboseMessage")

        self.qnn_err_handler = QnnErrHandling(
            errorGetMessage,
            errorGetVerboseMessage,
            errorFreeVerboseMessage
        )


class SystemProvider:
    def __init__(self, provider_ptr: POINTER(QnnSystemInterface_t)):
        self._p = provider_ptr.contents
        self._iface = None

        self.make_callable_system()

    def info(self):
        name = self._p.providerName.decode() if self._p.providerName else "(null)"
        sv = self._p.systemApiVersion

        return {
            "addr":         ctypes.addressof(self._p),
            "name_ptr":     (ctypes.cast(self._p.providerName, c_void_p).value or 0),
            "systemApi":    f"{int(sv.major)}.{int(sv.minor)}.{int(sv.patch)}",
            "backendId":    int(self._p.backendId),
            "providerName": name,
        }

    def __interface(self) -> QNN_SYSTEM_INTERFACE_VER_TYPE:
        if self._iface is None:
            # SAFER: the system interface table follows the struct BY VALUE.
            base = ctypes.addressof(self._p)
            iface_addr = base + ctypes.sizeof(QnnSystemInterface_t)
            iface_ptr = ctypes.cast(ctypes.c_void_p(
                iface_addr), POINTER(QNN_SYSTEM_INTERFACE_VER_TYPE))
            self._iface = iface_ptr.contents

        return self._iface

    def make_callable_system(self):
        # ensure this returns the system interface struct
        self.__iface = self.__interface()

        if not self._ptrs_look_sane(self.__iface):
            raise RuntimeError(
                "System interface pointers look invalid. "
                "Ensure QNN System table layout matches this struct"
                "or share headers to refine ctypes layout."
            )

        # System Context
        self.systemContextCreate = Binder.callable(
            self.__iface.systemContextCreate,
            QnnSystemContext_CreateFn_t,
            "systemContextCreate"
        )
        self.systemContextGetBinaryInfo = Binder.callable(
            self.__iface.systemContextGetBinaryInfo,
            QnnSystemContext_GetBinaryInfoFn_t,
            "systemContextGetBinaryInfo"
        )
        self.systemContextGetMetaData = Binder.callable(
            self.__iface.systemContextGetMetaData,
            QnnSystemContext_GetMetaDataFn_t,
            "systemContextGetMetaData"
        )
        self.systemContextFree = Binder.callable(
            self.__iface.systemContextFree,
            QnnSystemContext_FreeFn_t,
            "systemContextFree"
        )

        # System Tensor
        self.systemTensorGetMemoryFootprint = Binder.callable(
            self.__iface.systemTensorGetMemoryFootprint,
            QnnSystemTensor_getMemoryFootprintFn_t,
            "systemTensorGetMemoryFootprint"
        )

        # System Log
        self.systemLogCreate = Binder.callable(
            self.__iface.systemLogCreate,
            QnnSystemLog_createFn_t,
            "systemLogCreate"
        )
        self.systemLogSetLogLevel = Binder.callable(
            self.__iface.systemLogSetLogLevel,
            QnnSystemLog_setLogLevelFn_t,
            "systemLogSetLogLevel"
        )
        self.systemLogFree = Binder.callable(
            self.__iface.systemLogFree,
            QnnSystemLog_freeFn_t,
            "systemLogFree"
        )

        # System DLC
        self.systemDlcCreateFromFile = Binder.callable(
            self.__iface.systemDlcCreateFromFile,
            QnnSystemDlc_createFromFileFn_t,
            "systemDlcCreateFromFile"
        )
        self.systemDlcCreateFromBinary = Binder.callable(
            self.__iface.systemDlcCreateFromBinary,
            QnnSystemDlc_createFromBinaryFn_t,
            "systemDlcCreateFromBinary"
        )
        self.systemDlcComposeGraphs = Binder.callable(
            self.__iface.systemDlcComposeGraphs,
            QnnSystemDlc_composeGraphsFn_t,
            "systemDlcComposeGraphs"
        )
        self.systemDlcGetOpMappings = Binder.callable(
            self.__iface.systemDlcGetOpMappings,
            QnnSystemDlc_getOpMappingsFn_t,
            "systemDlcGetOpMappings"
        )
        self.systemDlcFree = Binder.callable(
            self.__iface.systemDlcFree,
            QnnSystemDlc_freeFn_t,
            "systemDlcFree"
        )

        # System Profile
        self.systemProfileCreateSerializationTarget = Binder.callable(
            self.__iface.systemProfileCreateSerializationTarget,
            QnnSystemProfile_createSerializationTargetFn_t,
            "systemProfileCreateSerializationTarget"
        )
        self.systemProfileSerializeEventData = Binder.callable(
            self.__iface.systemProfileSerializeEventData,
            QnnSystemProfile_serializeEventDataFn_t,
            "systemProfileSerializeEventData"
        )
        self.systemProfileFreeSerializationTarget = Binder.callable(
            self.__iface.systemProfileFreeSerializationTarget,
            QnnSystemProfile_freeSerializationTargetFn_t,
            "systemProfileFreeSerializationTarget"
        )

    def _ptrs_look_sane(self, iface: QNN_SYSTEM_INTERFACE_VER_TYPE) -> bool:
        ptrs = [
            iface.systemLogCreate, iface.systemLogFree,
            iface.systemContextCreate, iface.systemContextFree
        ]

        return all(p is not None and p >= 0x1000 for p in ptrs)
