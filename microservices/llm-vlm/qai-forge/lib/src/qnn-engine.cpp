// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// QNNEngine — Layer 4 wrapper around the QNN C API
//
// Translated from the GStreamer ml-qnn-engine.cc reference implementation.
// Supports only the cached context binary (.bin) path — the production
// deployment model for pre-compiled QNN graphs.
//
// QNN API access pattern:
//   1. dlopen(backend_lib)  → QnnInterface_getProviders → interface
//   2. dlopen(sys_lib)      → QnnSystemInterface_getProviders → sysinterface
//   3. logCreate / backendCreate / profileCreate / deviceCreate
//   4. systemContextGetBinaryInfo → GraphInfo_t extraction
//   5. contextCreateFromBinary → graphRetrieve
//   6. graphExecute (per inference call)
// ─────────────────────────────────────────────────────────────────────────────

#include "qnn-engine.hpp"

#include <dlfcn.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <numeric>

// QNN SDK headers — resolved via GENIE_INCLUDE_PATH at build time
#include <QnnInterface.h>
#include <System/QnnSystemInterface.h>
#include <System/QnnSystemContext.h>

// ─────────────────────────────────────────────────────────────────────────────
// Tensor version compatibility macros (mirrors ml-qnn-engine.cc)
// ─────────────────────────────────────────────────────────────────────────────

#if defined(QNN_TENSOR_V2_INIT)
  #define QNN_GET_TENSOR(t)                  ((t)->v2)
  #define QNN_TENSOR_VERSION_OK(t)           \
      (((t)->version == QNN_TENSOR_VERSION_1) || ((t)->version == QNN_TENSOR_VERSION_2))
#elif defined(QNN_TENSOR_V1_INIT)
  #define QNN_GET_TENSOR(t)                  ((t)->v1)
  #define QNN_TENSOR_VERSION_OK(t)           ((t)->version == QNN_TENSOR_VERSION_1)
#else
  // Fallback: assume v1
  #define QNN_GET_TENSOR(t)                  ((t)->v1)
  #define QNN_TENSOR_VERSION_OK(t)           ((t)->version == QNN_TENSOR_VERSION_1)
#endif

#if defined(QNN_SYSTEM_CONTEXT_BINARY_INFO_V3_INIT)
  #define QNN_GET_BINARY_INFO(b)             ((b)->contextBinaryInfoV3)
  #define QNN_BINARY_INFO_VERSION_OK(b)      \
      (((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) || \
       ((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) || \
       ((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3))
#elif defined(QNN_SYSTEM_CONTEXT_BINARY_INFO_V2_INIT)
  #define QNN_GET_BINARY_INFO(b)             ((b)->contextBinaryInfoV2)
  #define QNN_BINARY_INFO_VERSION_OK(b)      \
      (((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) || \
       ((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2))
#else
  #define QNN_GET_BINARY_INFO(b)             ((b)->contextBinaryInfoV1)
  #define QNN_BINARY_INFO_VERSION_OK(b)      \
      ((b)->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1)
#endif

#if defined(QNN_SYSTEM_CONTEXT_GRAPH_INFO_V3_INIT)
  #define QNN_GET_GRAPH_INFO(g)              ((g)->graphInfoV3)
  #define QNN_GRAPH_INFO_VERSION_OK(g)       \
      (((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) || \
       ((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2) || \
       ((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_3))
#elif defined(QNN_SYSTEM_CONTEXT_GRAPH_INFO_V2_INIT)
  #define QNN_GET_GRAPH_INFO(g)              ((g)->graphInfoV2)
  #define QNN_GRAPH_INFO_VERSION_OK(g)       \
      (((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1) || \
       ((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_2))
#else
  #define QNN_GET_GRAPH_INFO(g)              ((g)->graphInfoV1)
  #define QNN_GRAPH_INFO_VERSION_OK(g)       \
      ((g)->version == QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1)
#endif

#define QNN_TENSOR_NAME(t)       (QNN_GET_TENSOR(t).name)
#define QNN_TENSOR_RANK(t)       (QNN_GET_TENSOR(t).rank)
#define QNN_TENSOR_DIMS(t)       (QNN_GET_TENSOR(t).dimensions)
#define QNN_TENSOR_DTYPE(t)      (QNN_GET_TENSOR(t).dataType)
#define QNN_TENSOR_CLIENTBUF(t)  (QNN_GET_TENSOR(t).clientBuf)
#define QNN_TENSOR_QPARAMS(t)    (QNN_GET_TENSOR(t).quantizeParams)

// ─────────────────────────────────────────────────────────────────────────────
// Internal GraphInfo_t (mirrors the GStreamer plugin workaround)
// ─────────────────────────────────────────────────────────────────────────────
struct GraphInfo_t {
    Qnn_GraphHandle_t  graph;
    const char*        graphName;
    Qnn_Tensor_t*      inputTensors;
    uint32_t           numInputTensors;
    Qnn_Tensor_t*      outputTensors;
    uint32_t           numOutputTensors;
};

using QnnInterfaceGetProvidersFn    = decltype(QnnInterface_getProviders);
using QnnSysInterfaceGetProvidersFn = decltype(QnnSystemInterface_getProviders);

// ─────────────────────────────────────────────────────────────────────────────
// QNNEngine::Impl — holds all QNN handles and state
// ─────────────────────────────────────────────────────────────────────────────
struct QNNEngine::Impl {
    // Library handles
    void*                          libhandle    = nullptr;
    void*                          syslibhandle = nullptr;

    // QNN interfaces
    QNN_INTERFACE_VER_TYPE         interface    = {};
    QNN_SYSTEM_INTERFACE_VER_TYPE  sysinterface = {};

    // QNN handles
    Qnn_LogHandle_t                logger       = nullptr;
    Qnn_ProfileHandle_t            profiler     = nullptr;
    Qnn_DeviceHandle_t             device       = nullptr;
    Qnn_BackendHandle_t            backend      = nullptr;
    Qnn_ContextHandle_t            context      = nullptr;
    QnnSystemContext_Handle_t      sysctx       = nullptr;

    // Graph info (from binary)
    GraphInfo_t**                  graph_infos  = nullptr;
    uint32_t                       n_graphs     = 0;

    // Tensor specs (populated during init)
    std::vector<QNNTensorSpec>     input_specs;
    std::vector<QNNTensorSpec>     output_specs;

    // Binary buffer (kept alive for the lifetime of the context)
    std::vector<char>              binary_buf;

    ~Impl() { cleanup(); }

    void cleanup() {
        if (graph_infos) {
            if (sysctx && sysinterface.systemContextFree)
                sysinterface.systemContextFree(sysctx);
            // Free the flat array allocated in populateGraphInfo
            if (n_graphs > 0 && graph_infos[0])
                delete[] graph_infos[0];
            delete[] graph_infos;
            graph_infos = nullptr;
            n_graphs    = 0;
            sysctx      = nullptr;
        }
        if (interface.contextFree && context)
            interface.contextFree(context, nullptr);
        if (interface.deviceFree && device)
            interface.deviceFree(device);
        if (interface.profileFree && profiler)
            interface.profileFree(profiler);
        if (interface.backendFree && backend)
            interface.backendFree(backend);
        if (interface.logFree && logger)
            interface.logFree(logger);
        if (syslibhandle) { dlclose(syslibhandle); syslibhandle = nullptr; }
        if (libhandle)    { dlclose(libhandle);    libhandle    = nullptr; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void* loadSymbol(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym)
        throw std::runtime_error(std::string("QNNEngine: symbol not found: ") + name
                                 + " (" + dlerror() + ")");
    return sym;
}

static void qnnLogCallback(const char* fmt, QnnLog_Level_t level,
                            uint64_t /*ts*/, va_list args) {
    if (level <= QNN_LOG_LEVEL_WARN) {
        char buf[512];
        vsnprintf(buf, sizeof(buf), fmt, args);
        std::cerr << "[QNN] " << buf << "\n";
    }
}

static size_t qnnDtypeBytes(Qnn_DataType_t dt) {
    switch (dt) {
        case QNN_DATATYPE_FLOAT_32:
        case QNN_DATATYPE_INT_32:
        case QNN_DATATYPE_UINT_32:
        case QNN_DATATYPE_UFIXED_POINT_32:
        case QNN_DATATYPE_SFIXED_POINT_32:
            return 4;
        case QNN_DATATYPE_FLOAT_16:
        case QNN_DATATYPE_INT_16:
        case QNN_DATATYPE_UINT_16:
        case QNN_DATATYPE_UFIXED_POINT_16:
        case QNN_DATATYPE_SFIXED_POINT_16:
            return 2;
        case QNN_DATATYPE_INT_8:
        case QNN_DATATYPE_UINT_8:
        case QNN_DATATYPE_UFIXED_POINT_8:
        case QNN_DATATYPE_SFIXED_POINT_8:
        case QNN_DATATYPE_BOOL_8:
            return 1;
        case QNN_DATATYPE_INT_64:
        case QNN_DATATYPE_UINT_64:
            return 8;
        default:
            return 4;
    }
}

static size_t tensorByteSize(const Qnn_Tensor_t* t) {
    size_t n = qnnDtypeBytes(QNN_TENSOR_DTYPE(t));
    for (uint32_t i = 0; i < QNN_TENSOR_RANK(t); ++i)
        n *= QNN_TENSOR_DIMS(t)[i];
    return n;
}

// KFServing v2 datatype string for a given Qnn_DataType_t — mirrors
// snpeEncodingToString()/elementTypeToDtype() in the SNPE/LiteRT workers so
// tensorDataTypeFromString() (qai_forge/dto/TensorDTOs.h) resolves the real
// dtype instead of silently defaulting to FLOAT32 for quantized tensors.
static std::string qnnDtypeToString(Qnn_DataType_t dt) {
    switch (dt) {
        case QNN_DATATYPE_FLOAT_32:         return "FP32";
        case QNN_DATATYPE_FLOAT_16:         return "FP16";
        case QNN_DATATYPE_INT_32:           return "INT32";
        case QNN_DATATYPE_UINT_32:          return "UINT32";
        case QNN_DATATYPE_UFIXED_POINT_32:  return "UINT32";
        case QNN_DATATYPE_SFIXED_POINT_32:  return "INT32";
        case QNN_DATATYPE_INT_16:           return "INT16";
        case QNN_DATATYPE_UINT_16:          return "UINT16";
        case QNN_DATATYPE_UFIXED_POINT_16:  return "UINT16";
        case QNN_DATATYPE_SFIXED_POINT_16:  return "INT16";
        case QNN_DATATYPE_INT_8:            return "INT8";
        case QNN_DATATYPE_UINT_8:           return "UINT8";
        case QNN_DATATYPE_UFIXED_POINT_8:   return "UINT8";
        case QNN_DATATYPE_SFIXED_POINT_8:   return "INT8";
        case QNN_DATATYPE_BOOL_8:           return "BOOL";
        case QNN_DATATYPE_INT_64:           return "INT32"; // no 64-bit OIP type
        case QNN_DATATYPE_UINT_64:          return "UINT32";
        default:                            return "FP32";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// QNNEngine constructor
// ─────────────────────────────────────────────────────────────────────────────

QNNEngine::QNNEngine(const std::string& model_file,
                     const std::string& backend_lib,
                     const std::string& sys_lib)
    : impl_(std::make_unique<Impl>())
{
    auto& m = *impl_;

    // ── 1. Load backend library ───────────────────────────────────────────────
    m.libhandle = dlopen(backend_lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m.libhandle)
        throw std::runtime_error("QNNEngine: failed to open backend '" + backend_lib
                                 + "': " + dlerror());

    auto* GetProviders = reinterpret_cast<QnnInterfaceGetProvidersFn*>(
        loadSymbol(m.libhandle, "QnnInterface_getProviders"));

    const QnnInterface_t** providers = nullptr;
    uint32_t n_providers = 0;
    if (GetProviders(&providers, &n_providers) != QNN_SUCCESS || !providers || !n_providers)
        throw std::runtime_error("QNNEngine: QnnInterface_getProviders failed");

    m.interface = providers[0]->QNN_INTERFACE_VER_NAME;

    // ── 2. Load system library ────────────────────────────────────────────────
    m.syslibhandle = dlopen(sys_lib.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m.syslibhandle)
        throw std::runtime_error("QNNEngine: failed to open sys lib '" + sys_lib
                                 + "': " + dlerror());

    auto* GetSysProviders = reinterpret_cast<QnnSysInterfaceGetProvidersFn*>(
        loadSymbol(m.syslibhandle, "QnnSystemInterface_getProviders"));

    const QnnSystemInterface_t** sys_providers = nullptr;
    uint32_t n_sys = 0;
    if (GetSysProviders(&sys_providers, &n_sys) != QNN_SUCCESS || !sys_providers || !n_sys)
        throw std::runtime_error("QNNEngine: QnnSystemInterface_getProviders failed");

    m.sysinterface = sys_providers[0]->QNN_SYSTEM_INTERFACE_VER_NAME;

    // ── 3. Initialize: log → backend → profile → device ──────────────────────
    if (m.interface.logCreate(qnnLogCallback, QNN_LOG_LEVEL_WARN, &m.logger) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: logCreate failed");

    const QnnBackend_Config_t** bknd_cfg = nullptr;
    if (m.interface.backendCreate(m.logger, bknd_cfg, &m.backend) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: backendCreate failed");

    if (m.interface.profileCreate(m.backend, QNN_PROFILE_LEVEL_BASIC, &m.profiler) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: profileCreate failed");

    // Device creation is optional (some backends don't support it)
    Qnn_ErrorHandle_t dev_err = m.interface.deviceCreate(
        m.logger, nullptr, &m.device);
    if (dev_err != QNN_SUCCESS && dev_err != QNN_DEVICE_ERROR_UNSUPPORTED_FEATURE)
        throw std::runtime_error("QNNEngine: deviceCreate failed");

    // ── 4. Read context binary ────────────────────────────────────────────────
    std::ifstream f(model_file, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        throw std::runtime_error("QNNEngine: cannot open model file: " + model_file);

    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    m.binary_buf.resize(static_cast<size_t>(sz));
    if (!f.read(m.binary_buf.data(), sz))
        throw std::runtime_error("QNNEngine: failed to read model file: " + model_file);

    // ── 5. Inspect binary info ────────────────────────────────────────────────
    if (m.sysinterface.systemContextCreate(&m.sysctx) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: systemContextCreate failed");

    const QnnSystemContext_BinaryInfo_t* binary_info = nullptr;
    Qnn_ContextBinarySize_t              binary_info_size = 0;

    if (m.sysinterface.systemContextGetBinaryInfo(
            m.sysctx,
            static_cast<void*>(m.binary_buf.data()),
            static_cast<uint64_t>(m.binary_buf.size()),
            &binary_info,
            &binary_info_size) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: systemContextGetBinaryInfo failed");

    if (!QNN_BINARY_INFO_VERSION_OK(binary_info))
        throw std::runtime_error("QNNEngine: unsupported binary info version");

    // ── 6. Populate GraphInfo_t from binary info ──────────────────────────────
    m.n_graphs = QNN_GET_BINARY_INFO(binary_info).numGraphs;
    QnnSystemContext_GraphInfo_t* graphs = QNN_GET_BINARY_INFO(binary_info).graphs;

    m.graph_infos = new GraphInfo_t*[m.n_graphs];
    GraphInfo_t* arr = new GraphInfo_t[m.n_graphs];

    for (uint32_t i = 0; i < m.n_graphs; ++i) {
        if (!QNN_GRAPH_INFO_VERSION_OK(&graphs[i]))
            throw std::runtime_error("QNNEngine: unsupported graph info version");

        arr[i].graphName         = QNN_GET_GRAPH_INFO(&graphs[i]).graphName;
        arr[i].numInputTensors   = QNN_GET_GRAPH_INFO(&graphs[i]).numGraphInputs;
        arr[i].inputTensors      = QNN_GET_GRAPH_INFO(&graphs[i]).graphInputs;
        arr[i].numOutputTensors  = QNN_GET_GRAPH_INFO(&graphs[i]).numGraphOutputs;
        arr[i].outputTensors     = QNN_GET_GRAPH_INFO(&graphs[i]).graphOutputs;
        m.graph_infos[i]         = arr + i;
    }

    // ── 7. Create context from binary ─────────────────────────────────────────
    const QnnContext_Config_t** ctx_cfg = nullptr;
    if (m.interface.contextCreateFromBinary(
            m.backend, m.device, ctx_cfg,
            static_cast<void*>(m.binary_buf.data()),
            static_cast<uint64_t>(m.binary_buf.size()),
            &m.context, m.profiler) != QNN_SUCCESS)
        throw std::runtime_error("QNNEngine: contextCreateFromBinary failed");

    // ── 8. Retrieve graph handles ─────────────────────────────────────────────
    for (uint32_t i = 0; i < m.n_graphs; ++i) {
        if (m.interface.graphRetrieve(
                m.context,
                m.graph_infos[i]->graphName,
                &m.graph_infos[i]->graph) != QNN_SUCCESS)
            throw std::runtime_error(
                std::string("QNNEngine: graphRetrieve failed for graph: ")
                + m.graph_infos[i]->graphName);
    }

    // ── 9. Build tensor specs from graph 0 ────────────────────────────────────
    const GraphInfo_t* g = m.graph_infos[0];

    for (uint32_t i = 0; i < g->numInputTensors; ++i) {
        const Qnn_Tensor_t* t = &g->inputTensors[i];
        if (!QNN_TENSOR_VERSION_OK(t))
            throw std::runtime_error("QNNEngine: unsupported input tensor version");

        QNNTensorSpec spec;
        spec.name  = QNN_TENSOR_NAME(t);
        spec.dtype = static_cast<int>(QNN_TENSOR_DTYPE(t));
        spec.dtype_str = qnnDtypeToString(QNN_TENSOR_DTYPE(t));
        for (uint32_t d = 0; d < QNN_TENSOR_RANK(t); ++d)
            spec.shape.push_back(QNN_TENSOR_DIMS(t)[d]);
        spec.bytes = tensorByteSize(t);

        // Quantization params (if applicable)
        auto& qp = QNN_TENSOR_QPARAMS(t);
        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            spec.scale  = qp.scaleOffsetEncoding.scale;
            spec.offset = qp.scaleOffsetEncoding.offset;
        }
        m.input_specs.push_back(std::move(spec));
    }

    for (uint32_t i = 0; i < g->numOutputTensors; ++i) {
        const Qnn_Tensor_t* t = &g->outputTensors[i];
        if (!QNN_TENSOR_VERSION_OK(t))
            throw std::runtime_error("QNNEngine: unsupported output tensor version");

        QNNTensorSpec spec;
        spec.name  = QNN_TENSOR_NAME(t);
        spec.dtype = static_cast<int>(QNN_TENSOR_DTYPE(t));
        spec.dtype_str = qnnDtypeToString(QNN_TENSOR_DTYPE(t));
        for (uint32_t d = 0; d < QNN_TENSOR_RANK(t); ++d)
            spec.shape.push_back(QNN_TENSOR_DIMS(t)[d]);
        spec.bytes = tensorByteSize(t);

        auto& qp = QNN_TENSOR_QPARAMS(t);
        if (qp.encodingDefinition == QNN_DEFINITION_DEFINED &&
            qp.quantizationEncoding == QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
            spec.scale  = qp.scaleOffsetEncoding.scale;
            spec.offset = qp.scaleOffsetEncoding.offset;
        }
        m.output_specs.push_back(std::move(spec));
    }
}

QNNEngine::~QNNEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<QNNTensorSpec>& QNNEngine::inputSpecs() const {
    return impl_->input_specs;
}

const std::vector<QNNTensorSpec>& QNNEngine::outputSpecs() const {
    return impl_->output_specs;
}

// ─────────────────────────────────────────────────────────────────────────────
// infer()
// ─────────────────────────────────────────────────────────────────────────────

void QNNEngine::infer(const std::vector<const uint8_t*>& input_ptrs,
                      const std::vector<size_t>&          input_sizes,
                      const std::vector<uint8_t*>&         output_ptrs,
                      const std::vector<size_t>&           output_sizes)
{
    auto& m = *impl_;
    const GraphInfo_t* g = m.graph_infos[0];

    if (input_ptrs.size() != g->numInputTensors)
        throw std::runtime_error("QNNEngine::infer: wrong number of inputs");
    if (output_ptrs.size() != g->numOutputTensors)
        throw std::runtime_error("QNNEngine::infer: wrong number of outputs");

    // Set input clientBuf pointers
    for (uint32_t i = 0; i < g->numInputTensors; ++i) {
        Qnn_Tensor_t* t = &g->inputTensors[i];
        QNN_TENSOR_CLIENTBUF(t).data     = const_cast<uint8_t*>(input_ptrs[i]);
        QNN_TENSOR_CLIENTBUF(t).dataSize = static_cast<uint32_t>(input_sizes[i]);
    }

    // Set output clientBuf pointers
    for (uint32_t i = 0; i < g->numOutputTensors; ++i) {
        Qnn_Tensor_t* t = &g->outputTensors[i];
        QNN_TENSOR_CLIENTBUF(t).data     = output_ptrs[i];
        QNN_TENSOR_CLIENTBUF(t).dataSize = static_cast<uint32_t>(output_sizes[i]);
    }

    // Execute
    Qnn_ErrorHandle_t err = m.interface.graphExecute(
        g->graph,
        g->inputTensors,  g->numInputTensors,
        g->outputTensors, g->numOutputTensors,
        m.profiler, nullptr);

    if (err != QNN_GRAPH_NO_ERROR)
        throw std::runtime_error("QNNEngine::infer: graphExecute failed, error="
                                 + std::to_string(QNN_GET_ERROR_CODE(err)));
}
