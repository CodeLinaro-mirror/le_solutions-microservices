// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge_c_api.cpp — Flat C API Implementation
//
// Bridges the stable C ABI to QaiForge (the unified inference facade).
// Compiled into libqai_forge.so — no separate library needed.
//
// Memory contract:
//   - response_json_out and error_out are heap-allocated (malloc).
//     Callers must free them with qai_forge_free_string().
//   - chunk_json passed to the streaming callback is stack-allocated.
//     Callers must NOT free it; copy if needed beyond the callback scope.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/c_api/qai_forge_c_api.h"
#include "qai_forge/QaiForge.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <cstdlib>
#include <iostream>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

// Heap-allocate a copy of a std::string as a C string.
static char* alloc_str(const std::string& s) {
    char* p = static_cast<char*>(malloc(s.size() + 1));
    if (p) memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// Serialize a StandardResponse to a JSON string.
static std::string serialize_response(const StandardResponse& resp) {
    json j;
    j["id"]               = resp.id;
    j["model"]            = resp.model;
    j["content"]          = resp.content.value_or("");
    j["finish_reason"]    = resp.finish_reason;
    j["prompt_tokens"]    = resp.prompt_tokens;
    j["completion_tokens"] = resp.completion_tokens;
    j["total_tokens"]     = resp.total_tokens;
    if (resp.reasoning_content.has_value())
        j["reasoning_content"] = resp.reasoning_content.value();
    if (resp.tool_calls.has_value() && !resp.tool_calls.value().empty())
        j["tool_calls"] = resp.tool_calls.value();
    return j.dump();
}

// Serialize a StreamChunk to a JSON string.
static std::string serialize_chunk(const StreamChunk& chunk) {
    json j;
    j["id"]    = chunk.id;
    j["model"] = chunk.model;
    if (chunk.content_delta.has_value())
        j["content_delta"] = chunk.content_delta.value();
    if (chunk.reasoning_content.has_value())
        j["reasoning_delta"] = chunk.reasoning_content.value();
    if (chunk.finish_reason.has_value())
        j["finish_reason"] = chunk.finish_reason.value();
    if (chunk.role.has_value())
        j["role"] = chunk.role.value();
    return j.dump();
}

// Serialize a TensorInferenceResponse to a JSON string.
static std::string serialize_tensor_response(const TensorInferenceResponse& resp) {
    json j;
    j["id"]         = resp.request_id;
    j["model_name"] = resp.model;
    json outputs = json::array();
    for (const auto& out : resp.outputs) {
        json o;
        o["name"]     = out.name;
        o["datatype"] = tensorDataTypeToString(out.dtype);
        json shape = json::array();
        for (auto d : out.shape) shape.push_back(d);
        o["shape"] = shape;
        // Encode raw bytes as base64 or array — use array for simplicity
        json data = json::array();
        for (uint8_t b : out.data) data.push_back(b);
        o["data"] = data;
        outputs.push_back(o);
    }
    j["outputs"] = outputs;
    return j.dump();
}

// ─────────────────────────────────────────────────────────────────────────────
// C API implementation
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

// ── Generative AI ─────────────────────────────────────────────────────────────

int qai_forge_generate(const char*  request_json,
                       char**       response_json_out,
                       char**       error_out) {
    if (error_out)         *error_out         = nullptr;
    if (response_json_out) *response_json_out = nullptr;

    if (!request_json) {
        if (error_out) *error_out = alloc_str("request_json must not be NULL");
        return -1;
    }

    try {
        json j = json::parse(request_json);
        auto req = CreateChatCompletionRequest::from_json(j);

        qai_forge::GenerateOptions opts;
        opts.session_id = req.user.value_or("");

        StandardResponse resp =
            qai_forge::QaiForge::getInstance().generate(req, opts);

        if (response_json_out)
            *response_json_out = alloc_str(serialize_response(resp));
        return 0;

    } catch (const GenAIException& e) {
        if (error_out) *error_out = alloc_str(e.message);
        return e.http_status;
    } catch (const std::exception& e) {
        if (error_out) *error_out = alloc_str(e.what());
        return -1;
    } catch (...) {
        if (error_out) *error_out = alloc_str("Unknown error in qai_forge_generate");
        return -1;
    }
}

int qai_forge_generate_stream(const char*           request_json,
                               qai_forge_stream_cb_t callback,
                               void*                 user_data,
                               char**                error_out) {
    if (error_out) *error_out = nullptr;

    if (!request_json) {
        if (error_out) *error_out = alloc_str("request_json must not be NULL");
        return -1;
    }
    if (!callback) {
        if (error_out) *error_out = alloc_str("callback must not be NULL");
        return -1;
    }

    try {
        json j = json::parse(request_json);
        auto req = CreateChatCompletionRequest::from_json(j);

        qai_forge::GenerateOptions opts;
        opts.session_id = req.user.value_or("");

        // Use new async API with callbacks
        qai_forge::StreamCallbacks callbacks;

        callbacks.onToken = [callback, user_data](const StreamChunk& chunk) {
            // chunk_json is stack-allocated — valid only during this call
            std::string s = serialize_chunk(chunk);
            callback(s.c_str(), user_data);
        };

        // For C API, we don't need onComplete, onError, onCancelled
        // since this is a fire-and-forget interface
        callbacks.onComplete = [](const StandardResponse&) {};
        callbacks.onError = [](const GenAIException&) {};
        callbacks.onCancelled = []() {};

        qai_forge::QaiForge::getInstance().generateStream(
            req,
            std::move(callbacks),
            opts);

        return 0;

    } catch (const GenAIException& e) {
        if (error_out) *error_out = alloc_str(e.message);
        return e.http_status;
    } catch (const std::exception& e) {
        if (error_out) *error_out = alloc_str(e.what());
        return -1;
    } catch (...) {
        if (error_out) *error_out = alloc_str("Unknown error in qai_forge_generate_stream");
        return -1;
    }
}

// ── Predictive AI ─────────────────────────────────────────────────────────────

int qai_forge_infer(const char*  request_json,
                    char**       response_json_out,
                    char**       error_out) {
    if (error_out)         *error_out         = nullptr;
    if (response_json_out) *response_json_out = nullptr;

    if (!request_json) {
        if (error_out) *error_out = alloc_str("request_json must not be NULL");
        return -1;
    }

    try {
        json j = json::parse(request_json);

        TensorInferenceRequest req;
        req.model      = j.value("model", "");
        req.request_id = j.value("id", "");

        if (j.contains("inputs") && j["inputs"].is_array()) {
            for (const auto& inp : j["inputs"]) {
                InputTensor tensor;
                tensor.name  = inp.value("name", "");
                tensor.dtype = tensorDataTypeFromString(inp.value("datatype", "FP32"));
                if (inp.contains("shape") && inp["shape"].is_array()) {
                    for (auto d : inp["shape"]) tensor.shape.push_back(d.get<int64_t>());
                }
                if (inp.contains("data") && inp["data"].is_array()) {
                    for (auto b : inp["data"]) tensor.data.push_back(b.get<uint8_t>());
                }
                req.inputs.push_back(std::move(tensor));
            }
        }
        if (j.contains("outputs") && j["outputs"].is_array()) {
            for (const auto& o : j["outputs"]) {
                req.output_names.push_back(o.get<std::string>());
            }
        }

        TensorInferenceResponse resp =
            qai_forge::QaiForge::getInstance().infer(req);

        if (response_json_out)
            *response_json_out = alloc_str(serialize_tensor_response(resp));
        return 0;

    } catch (const GenAIException& e) {
        if (error_out) *error_out = alloc_str(e.message);
        return e.http_status;
    } catch (const std::exception& e) {
        if (error_out) *error_out = alloc_str(e.what());
        return -1;
    } catch (...) {
        if (error_out) *error_out = alloc_str("Unknown error in qai_forge_infer");
        return -1;
    }
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void qai_forge_start(void) {
    qai_forge::QaiForge::getInstance().start();
}

void qai_forge_shutdown(int force) {
    qai_forge::QaiForge::getInstance().shutdown(force != 0);
}

int qai_forge_cancel(const char* response_id) {
    if (!response_id) return 0;
    return qai_forge::QaiForge::getInstance().cancel(response_id) ? 1 : 0;
}

// ── Memory management ─────────────────────────────────────────────────────────

void qai_forge_free_string(char* str) {
    free(str);
}

const char* qai_forge_version(void) {
    return "1.0.0";
}

} // extern "C"
