// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// InferController — Layer 1 (KFServing v2 Inference Protocol)
//
// Responsibilities:
//   1. Parse KFServing v2 JSON request → TensorInferenceRequest (raw bytes)
//   2. Call IInferenceRouter::handleInfer() → TensorInferenceResponse
//   3. Format TensorInferenceResponse → KFServing v2 JSON response
//
// This layer MUST NOT contain any inference logic.
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/InferController.h"
#include "qai_forge/routing/IInferenceRouter.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static HttpResponsePtr make_error(int status, const std::string& message) {
    json body = {
        {"error", {
            {"message", message},
            {"code",    status}
        }}
    };
    auto resp = HttpResponse::newHttpJsonResponse(body.dump());
    resp->setStatusCode(static_cast<HttpStatusCode>(status));
    return resp;
}

static std::string generate_request_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "infer-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// KFServing v2 data encoding/decoding
//
// The "data" field is a flat JSON array of numbers.
// We convert between JSON numbers and raw bytes (little-endian).
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Decode a KFServing v2 "data" JSON array to raw bytes.
 * Supports FP32, FP16, INT8, UINT8, INT16, UINT16, INT32, UINT32, INT64, UINT64.
 */
static std::vector<uint8_t> decodeDataArray(const json& data_arr,
                                              const std::string& datatype) {
    std::vector<uint8_t> bytes;

    if (datatype == "FP32") {
        bytes.resize(data_arr.size() * 4);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            float v = data_arr[i].get<float>();
            std::memcpy(bytes.data() + i * 4, &v, 4);
        }
    } else if (datatype == "FP16") {
        // Store as uint16 (caller must handle FP16 conversion)
        bytes.resize(data_arr.size() * 2);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            uint16_t v = static_cast<uint16_t>(data_arr[i].get<float>() * 65504.0f);
            std::memcpy(bytes.data() + i * 2, &v, 2);
        }
    } else if (datatype == "INT8") {
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int8_t v = static_cast<int8_t>(data_arr[i].get<int>());
            bytes[i] = static_cast<uint8_t>(v);
        }
    } else if (datatype == "UINT8") {
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i)
            bytes[i] = static_cast<uint8_t>(data_arr[i].get<int>());
    } else if (datatype == "INT16") {
        bytes.resize(data_arr.size() * 2);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int16_t v = static_cast<int16_t>(data_arr[i].get<int>());
            std::memcpy(bytes.data() + i * 2, &v, 2);
        }
    } else if (datatype == "UINT16") {
        bytes.resize(data_arr.size() * 2);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            uint16_t v = static_cast<uint16_t>(data_arr[i].get<int>());
            std::memcpy(bytes.data() + i * 2, &v, 2);
        }
    } else if (datatype == "INT32") {
        bytes.resize(data_arr.size() * 4);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int32_t v = data_arr[i].get<int32_t>();
            std::memcpy(bytes.data() + i * 4, &v, 4);
        }
    } else if (datatype == "UINT32") {
        bytes.resize(data_arr.size() * 4);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            uint32_t v = data_arr[i].get<uint32_t>();
            std::memcpy(bytes.data() + i * 4, &v, 4);
        }
    } else if (datatype == "INT64") {
        bytes.resize(data_arr.size() * 8);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int64_t v = data_arr[i].get<int64_t>();
            std::memcpy(bytes.data() + i * 8, &v, 8);
        }
    } else if (datatype == "UINT64") {
        bytes.resize(data_arr.size() * 8);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            uint64_t v = data_arr[i].get<uint64_t>();
            std::memcpy(bytes.data() + i * 8, &v, 8);
        }
    } else {
        // Unknown datatype — treat as raw bytes (UINT8)
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i)
            bytes[i] = static_cast<uint8_t>(data_arr[i].get<int>());
    }

    return bytes;
}

/**
 * Encode raw bytes to a KFServing v2 "data" JSON array.
 */
static json encodeDataArray(const std::vector<uint8_t>& bytes,
                              const std::string& datatype) {
    json arr = json::array();

    if (datatype == "FP32") {
        size_t n = bytes.size() / 4;
        for (size_t i = 0; i < n; ++i) {
            float v;
            std::memcpy(&v, bytes.data() + i * 4, 4);
            arr.push_back(v);
        }
    } else if (datatype == "INT8") {
        for (uint8_t b : bytes) arr.push_back(static_cast<int8_t>(b));
    } else if (datatype == "UINT8") {
        for (uint8_t b : bytes) arr.push_back(b);
    } else if (datatype == "INT16") {
        size_t n = bytes.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            int16_t v;
            std::memcpy(&v, bytes.data() + i * 2, 2);
            arr.push_back(v);
        }
    } else if (datatype == "UINT16") {
        size_t n = bytes.size() / 2;
        for (size_t i = 0; i < n; ++i) {
            uint16_t v;
            std::memcpy(&v, bytes.data() + i * 2, 2);
            arr.push_back(v);
        }
    } else if (datatype == "INT32") {
        size_t n = bytes.size() / 4;
        for (size_t i = 0; i < n; ++i) {
            int32_t v;
            std::memcpy(&v, bytes.data() + i * 4, 4);
            arr.push_back(v);
        }
    } else if (datatype == "UINT32") {
        size_t n = bytes.size() / 4;
        for (size_t i = 0; i < n; ++i) {
            uint32_t v;
            std::memcpy(&v, bytes.data() + i * 4, 4);
            arr.push_back(v);
        }
    } else if (datatype == "INT64") {
        size_t n = bytes.size() / 8;
        for (size_t i = 0; i < n; ++i) {
            int64_t v;
            std::memcpy(&v, bytes.data() + i * 8, 8);
            arr.push_back(v);
        }
    } else if (datatype == "UINT64") {
        size_t n = bytes.size() / 8;
        for (size_t i = 0; i < n; ++i) {
            uint64_t v;
            std::memcpy(&v, bytes.data() + i * 8, 8);
            arr.push_back(v);
        }
    } else {
        // Unknown — return as UINT8
        for (uint8_t b : bytes) arr.push_back(b);
    }

    return arr;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/models/{model}/infer
// ─────────────────────────────────────────────────────────────────────────────

void InferController::infer(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& model_name)
{
    // Step 1: Parse JSON body
    json body;
    try {
        body = json::parse(req->getBody());
    } catch (...) {
        callback(make_error(400, "Invalid JSON body"));
        return;
    }

    // Step 2: Validate required fields
    if (!body.contains("inputs") || !body["inputs"].is_array() || body["inputs"].empty()) {
        callback(make_error(400, "Missing required field: 'inputs' (must be a non-empty array)"));
        return;
    }

    // Step 3: Build TensorInferenceRequest
    TensorInferenceRequest request;
    request.model      = model_name;
    request.request_id = body.value("id", generate_request_id());

    // Parse input tensors
    try {
        for (const auto& inp : body["inputs"]) {
            InputTensor tensor;
            tensor.name  = inp.value("name", "");
            tensor.dtype = tensorDataTypeFromString(inp.value("datatype", "FP32"));

            for (auto d : inp.value("shape", json::array()))
                tensor.shape.push_back(d.get<int64_t>());

            // Decode "data" array → raw bytes
            if (inp.contains("data") && inp["data"].is_array()) {
                tensor.data = decodeDataArray(inp["data"], inp.value("datatype", "FP32"));
            } else {
                callback(make_error(400,
                    "Input tensor '" + tensor.name + "' missing 'data' field"));
                return;
            }

            request.inputs.push_back(std::move(tensor));
        }
    } catch (const std::exception& e) {
        callback(make_error(400, std::string("Failed to parse inputs: ") + e.what()));
        return;
    }

    // Parse requested output names (optional)
    for (const auto& out : body.value("outputs", json::array())) {
        if (out.contains("name"))
            request.output_names.push_back(out["name"].get<std::string>());
    }

    // Step 4: Route to ConventionalAIOrchestrator via InferenceRouter
    try {
        TensorInferenceResponse result =
            IInferenceRouter::getInstance().handleInfer(request);

        // Step 5: Build KFServing v2 response
        json outputs_json = json::array();
        for (const auto& out : result.outputs) {
            json t;
            t["name"]     = out.name;
            t["datatype"] = tensorDataTypeToString(out.dtype);

            json shape_arr = json::array();
            for (auto d : out.shape) shape_arr.push_back(d);
            t["shape"] = shape_arr;

            // Encode raw bytes → "data" array
            t["data"] = encodeDataArray(out.data, tensorDataTypeToString(out.dtype));

            outputs_json.push_back(t);
        }

        json response_body = {
            {"id",         result.request_id},
            {"model_name", result.model},
            {"outputs",    outputs_json}
        };

        auto resp = HttpResponse::newHttpJsonResponse(response_body.dump());
        resp->setStatusCode(k200OK);
        callback(resp);

    } catch (const GenAIException& e) {
        // Map domain exceptions to HTTP status codes
        std::string msg = e.message;

        // Correct the error message for generative models
        if (e.code == GenAIErrorCode::INVALID_REQUEST &&
            msg.find("generative") != std::string::npos) {
            msg = "Model '" + model_name + "' is a generative model. "
                  "Use POST /v1/responses instead of /v2/models/{model}/infer.";
        }

        callback(make_error(e.http_status, msg));
    } catch (const std::exception& e) {
        callback(make_error(500, std::string("Internal server error: ") + e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/models/{model}
// ─────────────────────────────────────────────────────────────────────────────

void InferController::getModelInfo(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& model_name)
{
    auto& cfg = ModelConfigManager::getInstance();

    if (!cfg.validateModel(model_name)) {
        callback(make_error(404, "Model '" + model_name + "' not found. "
                                 "Check /v1/models for available models."));
        return;
    }

    std::string model_type = cfg.getModelType(model_name);
    if (model_type != "conventional") {
        callback(make_error(400,
            "Model '" + model_name + "' is a generative model. "
            "Use POST /v1/responses instead of /v2/models/{model}/infer."));
        return;
    }

    std::string runtime = cfg.getRuntime(model_name);
    const ModelConfig* mc = cfg.getModelConfig(model_name);

    // Build KFServing v2 model metadata response
    json response = {
        {"name",     model_name},
        {"versions", json::array({"1"})},
        {"platform", runtime},   // "qnn" or "snpe"
        {"inputs",   json::array()},
        {"outputs",  json::array()}
    };

    // Populate input/output specs from metadata.json if available
    if (mc) {
        response["ready"] = true;

        for (const auto& spec : mc->input_specs) {
            json shape_arr = json::array();
            for (auto d : spec.shape) shape_arr.push_back(d);
            response["inputs"].push_back({
                {"name", spec.name},
                {"datatype", spec.dtype},
                {"shape", shape_arr}
            });
        }

        for (const auto& spec : mc->output_specs) {
            json shape_arr = json::array();
            for (auto d : spec.shape) shape_arr.push_back(d);
            response["outputs"].push_back({
                {"name", spec.name},
                {"datatype", spec.dtype},
                {"shape", shape_arr}
            });
        }
    }

    auto resp = HttpResponse::newHttpJsonResponse(response.dump());
    resp->setStatusCode(k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/health/ready
// ─────────────────────────────────────────────────────────────────────────────

void InferController::healthReady(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    // Check if any models are loaded
    auto models = ModelConfigManager::getInstance().getAvailableModels();

    if (models.empty()) {
        auto resp = HttpResponse::newHttpJsonResponse(
            R"({"ready":false,"message":"No models loaded"})");
        resp->setStatusCode(k503ServiceUnavailable);
        callback(resp);
        return;
    }

    auto resp = HttpResponse::newHttpJsonResponse(R"({"ready":true})");
    resp->setStatusCode(k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/health/live
// ─────────────────────────────────────────────────────────────────────────────

void InferController::healthLive(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    // Always 200 — if we can respond, we're alive
    auto resp = HttpResponse::newHttpJsonResponse(R"({"live":true})");
    resp->setStatusCode(k200OK);
    callback(resp);
}
