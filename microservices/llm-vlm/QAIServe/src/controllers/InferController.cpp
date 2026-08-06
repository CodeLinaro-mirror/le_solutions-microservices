// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// InferController — OIP v2 Inference Endpoints
//
// Handles:
//   POST /v2/models/{model}/infer           — Predictive AI (JSON + binary ext)
//   POST /v2/models/{model}/generate        — Generative AI blocking
//   POST /v2/models/{model}/generate_stream — Generative AI SSE streaming
//   GET  /v2/health/ready
//   GET  /v2/health/live
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/InferController.h"
#include "oip/OipDTOs.h"
#include "oip/OipBinaryParser.h"
#include "oip/MultipartParser.h"
#include "qai_forge/routing/IInferenceRouter.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <cstring>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <iostream>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

HttpResponsePtr makeError(int status, const std::string& message) {
    json body = {{"error", {{"message", message}, {"code", status}}}};
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<HttpStatusCode>(status));
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

HttpResponsePtr makeJson(const json& body, HttpStatusCode code = k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

std::string generateRequestId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "infer-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

// ── KFServing v2 data encoding/decoding ──────────────────────────────────────

std::vector<uint8_t> decodeDataArray(const json& data_arr, const std::string& datatype) {
    std::vector<uint8_t> bytes;
    if (datatype == "FP32") {
        bytes.resize(data_arr.size() * 4);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            float v = data_arr[i].get<float>();
            std::memcpy(bytes.data() + i * 4, &v, 4);
        }
    } else if (datatype == "INT32") {
        bytes.resize(data_arr.size() * 4);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int32_t v = data_arr[i].get<int32_t>();
            std::memcpy(bytes.data() + i * 4, &v, 4);
        }
    } else if (datatype == "INT8") {
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i)
            bytes[i] = static_cast<uint8_t>(static_cast<int8_t>(data_arr[i].get<int>()));
    } else if (datatype == "UINT8") {
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i)
            bytes[i] = static_cast<uint8_t>(data_arr[i].get<int>());
    } else if (datatype == "INT16" || datatype == "UINT16") {
        bytes.resize(data_arr.size() * 2);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            uint16_t v = static_cast<uint16_t>(data_arr[i].get<int>());
            std::memcpy(bytes.data() + i * 2, &v, 2);
        }
    } else if (datatype == "INT64" || datatype == "UINT64") {
        bytes.resize(data_arr.size() * 8);
        for (size_t i = 0; i < data_arr.size(); ++i) {
            int64_t v = data_arr[i].get<int64_t>();
            std::memcpy(bytes.data() + i * 8, &v, 8);
        }
    } else {
        bytes.resize(data_arr.size());
        for (size_t i = 0; i < data_arr.size(); ++i)
            bytes[i] = static_cast<uint8_t>(data_arr[i].get<int>());
    }
    return bytes;
}

json encodeDataArray(const std::vector<uint8_t>& bytes, const std::string& datatype) {
    json arr = json::array();
    if (datatype == "FP32") {
        for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
            float v; std::memcpy(&v, bytes.data() + i, 4); arr.push_back(v);
        }
    } else if (datatype == "INT32") {
        for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
            int32_t v; std::memcpy(&v, bytes.data() + i, 4); arr.push_back(v);
        }
    } else if (datatype == "INT8") {
        for (uint8_t b : bytes) arr.push_back(static_cast<int8_t>(b));
    } else if (datatype == "UINT8") {
        for (uint8_t b : bytes) arr.push_back(b);
    } else if (datatype == "INT16") {
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            int16_t v; std::memcpy(&v, bytes.data() + i, 2); arr.push_back(v);
        }
    } else if (datatype == "UINT16") {
        for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
            uint16_t v; std::memcpy(&v, bytes.data() + i, 2); arr.push_back(v);
        }
    } else {
        for (uint8_t b : bytes) arr.push_back(b);
    }
    return arr;
}

// Build CreateChatCompletionRequest from OipGenerateRequest
CreateChatCompletionRequest buildChatRequest(
    const OipGenerateRequest& oip_req,
    const std::string& model_name)
{
    CreateChatCompletionRequest req;
    req.model  = model_name;
    req.stream = false;

    const auto& p = oip_req.parameters;
    req.max_completion_tokens = p.max_new_tokens;
    req.temperature           = p.temperature;
    req.top_p                 = p.top_p;
    req.top_k                 = p.top_k;

    // OIP stateless mode: use a unique ephemeral session ID
    // (empty user = stateless; non-empty = multi-turn via /v1/responses)
    if (!p.user.empty()) {
        req.user = p.user;
    }
    // else: no user field → ChatOrchestrator creates a fresh session

    if (oip_req.messages.has_value()) {
        // Server applies chat template
        req.messages = oip_req.messages.value();
    } else if (oip_req.text_input.has_value()) {
        // Raw prompt — wrap as a single user message
        // The orchestrator will feed it directly without re-applying the template
        req.messages = json::array({
            {{"role", "user"}, {"content", oip_req.text_input.value()}}
        });
        // Signal raw prompt mode via a custom field
        req.raw_prompt = oip_req.text_input.value();
    }

    return req;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/models/{model}/infer — Predictive AI
// ─────────────────────────────────────────────────────────────────────────────
void InferController::infer(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& model_name)
{
    // Validate model type
    auto& cfg = ModelConfigManager::getInstance();
    if (!cfg.validateModel(model_name)) {
        callback(makeError(404, "Model '" + model_name + "' not found."));
        return;
    }
    if (cfg.getModelType(model_name) != "predictive") {
        callback(makeError(400,
            "Model '" + model_name + "' is a generative model. "
            "Use POST /v2/models/" + model_name + "/generate instead."));
        return;
    }

    // Parse request — JSON or binary extension
    OipInferRequest oip_req;
    try {
        std::string infer_header = req->getHeader("Inference-Header-Content-Length");
        if (OipBinaryParser::isBinaryRequest(infer_header)) {
            size_t header_len = static_cast<size_t>(std::stoul(infer_header));
            oip_req = OipBinaryParser::parse(std::string(req->getBody()), header_len);
        } else {
            oip_req = OipBinaryParser::parseJson(std::string(req->getBody()));
        }
    } catch (const OipBinaryParseError& e) {
        callback(makeError(400, std::string("Request parse error: ") + e.what()));
        return;
    }

    if (oip_req.inputs.empty()) {
        callback(makeError(400, "Missing required field: 'inputs'"));
        return;
    }

    // Build TensorInferenceRequest
    TensorInferenceRequest request;
    request.model      = model_name;
    request.request_id = oip_req.id.empty() ? generateRequestId() : oip_req.id;

    for (const auto& inp : oip_req.inputs) {
        InputTensor tensor;
        tensor.name  = inp.name;
        tensor.dtype = tensorDataTypeFromString(inp.datatype);
        tensor.shape = inp.shape;
        tensor.data  = inp.data;
        request.inputs.push_back(std::move(tensor));
    }
    request.output_names = oip_req.outputs;

    // Route to PredictiveAIOrchestrator
    try {
        TensorInferenceResponse result =
            IInferenceRouter::getInstance().handleInfer(request);

        json outputs_json = json::array();
        for (const auto& out : result.outputs) {
            std::string dt = tensorDataTypeToString(out.dtype);
            json shape_arr = json::array();
            for (auto d : out.shape) shape_arr.push_back(d);
            outputs_json.push_back({
                {"name",     out.name},
                {"datatype", dt},
                {"shape",    shape_arr},
                {"data",     encodeDataArray(out.data, dt)},
            });
        }

        callback(makeJson({
            {"id",         result.request_id},
            {"model_name", result.model},
            {"outputs",    outputs_json},
        }));

    } catch (const GenAIException& e) {
        callback(makeError(e.http_status, e.message));
    } catch (const std::exception& e) {
        callback(makeError(500, std::string("Internal error: ") + e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/models/{model}/generate — Generative AI blocking (OIP stateless)
// ─────────────────────────────────────────────────────────────────────────────
void InferController::generate(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& model_name)
{
    auto& cfg = ModelConfigManager::getInstance();
    if (!cfg.validateModel(model_name)) {
        callback(makeError(404, "Model '" + model_name + "' not found."));
        return;
    }
    if (cfg.getModelType(model_name) != "generative") {
        callback(makeError(400,
            "Model '" + model_name + "' is a predictive model. "
            "Use POST /v2/models/" + model_name + "/infer instead."));
        return;
    }

    // Parse request — JSON or multipart (VLM)
    OipGenerateRequest oip_req;
    try {
        std::string content_type = req->getHeader("Content-Type");
        if (MultipartParser::isMultipart(content_type)) {
            std::string boundary = MultipartParser::extractBoundary(content_type);
            auto parts = MultipartParser::parse(std::string(req->getBody()), boundary);

            // "request" part contains the JSON
            auto it = parts.find("request");
            if (it == parts.end()) {
                callback(makeError(400, "Multipart request missing 'request' part"));
                return;
            }
            std::string json_str(it->second.data.begin(), it->second.data.end());
            oip_req = OipGenerateRequest::fromJson(json::parse(json_str));

            // Collect image parts: image_0, image_1, ...
            for (int i = 0; ; ++i) {
                auto img_it = parts.find("image_" + std::to_string(i));
                if (img_it == parts.end()) break;
                oip_req.images.push_back(img_it->second.data);
            }
        } else {
            oip_req = OipGenerateRequest::fromJson(json::parse(req->getBody()));
        }
    } catch (const std::exception& e) {
        callback(makeError(400, std::string("Request parse error: ") + e.what()));
        return;
    }

    if (!oip_req.text_input.has_value() && !oip_req.messages.has_value()) {
        callback(makeError(400, "Either 'text_input' or 'messages' is required."));
        return;
    }

    // Build ChatOrchestrator request
    CreateChatCompletionRequest chat_req = buildChatRequest(oip_req, model_name);

    // OIP stateless: reset KV cache before inference
    auto& orchestrator = ChatOrchestrator::getInstance();
    orchestrator.resetKvCache(model_name);

    try {
        StandardResponse resp = orchestrator.handleBlocking(chat_req);

        // OIP stateless: reset KV cache after inference
        orchestrator.resetKvCache(model_name);

        // Clean up ephemeral session (stateless mode)
        if (!oip_req.parameters.user.empty()) {
            orchestrator.deleteSession(resp.id);
        }

        OipGenerateResponse oip_resp;
        oip_resp.model_name        = model_name;
        oip_resp.text_output       = resp.content.value_or("");
        oip_resp.reasoning_output  = resp.reasoning_content.value_or("");
        oip_resp.finish_reason     = resp.finish_reason;
        oip_resp.prompt_tokens     = resp.prompt_tokens;
        oip_resp.completion_tokens = resp.completion_tokens;

        callback(makeJson(oip_resp.toJson()));

    } catch (const GenAIException& e) {
        orchestrator.resetKvCache(model_name); // ensure clean state on error
        callback(makeError(e.http_status, e.message));
    } catch (const std::exception& e) {
        orchestrator.resetKvCache(model_name);
        callback(makeError(500, std::string("Internal error: ") + e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/models/{model}/generate_stream — Generative AI SSE streaming
// ─────────────────────────────────────────────────────────────────────────────
void InferController::generateStream(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& model_name)
{
    auto& cfg = ModelConfigManager::getInstance();
    if (!cfg.validateModel(model_name)) {
        callback(makeError(404, "Model '" + model_name + "' not found."));
        return;
    }
    if (cfg.getModelType(model_name) != "generative") {
        callback(makeError(400,
            "Model '" + model_name + "' is a predictive model. "
            "Use POST /v2/models/" + model_name + "/infer instead."));
        return;
    }

    // Parse request
    OipGenerateRequest oip_req;
    try {
        std::string content_type = req->getHeader("Content-Type");
        if (MultipartParser::isMultipart(content_type)) {
            std::string boundary = MultipartParser::extractBoundary(content_type);
            auto parts = MultipartParser::parse(std::string(req->getBody()), boundary);
            auto it = parts.find("request");
            if (it == parts.end()) {
                callback(makeError(400, "Multipart request missing 'request' part"));
                return;
            }
            std::string json_str(it->second.data.begin(), it->second.data.end());
            oip_req = OipGenerateRequest::fromJson(json::parse(json_str));
            for (int i = 0; ; ++i) {
                auto img_it = parts.find("image_" + std::to_string(i));
                if (img_it == parts.end()) break;
                oip_req.images.push_back(img_it->second.data);
            }
        } else {
            oip_req = OipGenerateRequest::fromJson(json::parse(req->getBody()));
        }
    } catch (const std::exception& e) {
        callback(makeError(400, std::string("Request parse error: ") + e.what()));
        return;
    }

    if (!oip_req.text_input.has_value() && !oip_req.messages.has_value()) {
        callback(makeError(400, "Either 'text_input' or 'messages' is required."));
        return;
    }

    CreateChatCompletionRequest chat_req = buildChatRequest(oip_req, model_name);
    chat_req.stream = true;

    // OIP stateless: reset KV cache before inference
    auto& orchestrator = ChatOrchestrator::getInstance();
    orchestrator.resetKvCache(model_name);

    // Set up SSE response
    auto sse_resp = HttpResponse::newAsyncStreamResponse(
        [model_name, chat_req, &orchestrator, oip_req]
        (drogon::ResponseStreamPtr stream) mutable {
            int completion_tokens = 0;
            std::string session_id;

            try {
                orchestrator.handleStreaming(
                    chat_req,
                    [&stream, &model_name, &completion_tokens, &session_id]
                    (const StreamChunk& chunk) {
                        if (!session_id.empty()) session_id = chunk.id;

                        OipStreamChunk oip_chunk;
                        oip_chunk.model_name = model_name;

                        if (chunk.finish_reason.has_value() && !chunk.finish_reason->empty()) {
                            oip_chunk.finish_reason     = chunk.finish_reason.value();
                            oip_chunk.completion_tokens = completion_tokens;
                        } else {
                            if (chunk.content_delta.has_value()) {
                                oip_chunk.text_output = chunk.content_delta.value();
                                completion_tokens++;
                            }
                            if (chunk.reasoning_content.has_value()) {
                                oip_chunk.reasoning_output = chunk.reasoning_content.value();
                            }
                        }

                        std::string event = "data: " + oip_chunk.toJson().dump() + "\n\n";
                        stream->send(event);
                    });

                // OIP stateless: reset KV cache after inference
                orchestrator.resetKvCache(model_name);

                // Clean up ephemeral session
                if (!session_id.empty()) {
                    orchestrator.deleteSession(session_id);
                }

            } catch (const std::exception& e) {
                orchestrator.resetKvCache(model_name);
                std::string err_event = "data: {\"error\":\"" + std::string(e.what()) + "\"}\n\n";
                stream->send(err_event);
            }

            stream->send("data: [DONE]\n\n");
            stream->close();
        });

    sse_resp->setStatusCode(k200OK);
    sse_resp->addHeader("Content-Type", "text/event-stream");
    sse_resp->addHeader("Cache-Control", "no-cache");
    sse_resp->addHeader("X-Accel-Buffering", "no");
    callback(sse_resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/health/ready
// ─────────────────────────────────────────────────────────────────────────────
void InferController::healthReady(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto models = ModelConfigManager::getInstance().getAvailableModels();
    if (models.empty()) {
        callback(makeJson({{"ready", false}, {"message", "No models loaded"}},
                          k503ServiceUnavailable));
        return;
    }
    callback(makeJson({{"ready", true}}));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/health/live
// ─────────────────────────────────────────────────────────────────────────────
void InferController::healthLive(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    callback(makeJson({{"live", true}}));
}
