// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ModelsController — Implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/ModelsController.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "postproc/PostprocRegistry.h"

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <ctime>

// Note: ModelConfigManager.h already declares `using json = nlohmann::ordered_json`
// at file scope; do not redeclare here to avoid a conflicting-declaration error.

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

HttpResponsePtr jsonResp(const json& body, HttpStatusCode code = k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump(2));
    return resp;
}

// Build OIP tensor spec array from ModelConfig tensor specs
json buildTensorSpecs(const std::vector<ModelTensorSpec>& specs) {
    json arr = json::array();
    for (const auto& s : specs) {
        json t = {
            {"name",     s.name},
            {"datatype", s.dtype},
            {"shape",    s.shape},
        };
        arr.push_back(t);
    }
    return arr;
}

// Build OIP inputs/outputs for generative models (text + optional image)
json buildGenerativeInputs(bool supports_vision) {
    json inputs = json::array();
    inputs.push_back({
        {"name",     "text_input"},
        {"datatype", "BYTES"},
        {"shape",    json::array({-1})},
    });
    if (supports_vision) {
        inputs.push_back({
            {"name",     "image_input"},
            {"datatype", "BYTES"},
            {"shape",    json::array({-1})},
            {"optional", true},
        });
    }
    return inputs;
}

json buildGenerativeOutputs() {
    return json::array({
        {{"name", "text_output"}, {"datatype", "BYTES"}, {"shape", json::array({-1})}},
    });
}

// Serialize a postprocess's ParameterSchema list, keyed by parameter name,
// for GET /v2/postprocesses and the per-model extensions[] entry.
json buildParamsJson(const std::vector<ParameterSchema>& params) {
    json obj = json::object();
    for (const auto& p : params) {
        obj[p.name] = {
            {"type",        p.type},
            {"default",     p.default_val},
            {"description", p.description},
        };
    }
    return obj;
}

// Serialize a postprocess's supported tensor layouts.
// Each layout is a positional list of TensorExpectation.
json buildExpectsJson(const std::vector<std::vector<TensorExpectation>>& layouts) {
    json layouts_json = json::array();
    for (const auto& layout : layouts) {
        json tensors_json = json::array();
        for (const auto& t : layout) {
            json dtypes_json = json::array();
            for (auto dt : t.accepted_dtypes) dtypes_json.push_back(tensorDataTypeToString(dt));

            tensors_json.push_back({
                {"shape",  t.shape},
                {"dtypes", dtypes_json},
            });
        }
        layouts_json.push_back(tensors_json);
    }
    return layouts_json;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/models — OpenAI-compatible (generative models only)
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::listModelsV1(const HttpRequestPtr& /*req*/,
                                     std::function<void(const HttpResponsePtr&)>&& callback) {
    auto models = ModelConfigManager::getInstance().getAvailableModels();

    json data = json::array();
    for (const auto& m : models) {
        // OpenAI /v1/models only lists generative models
        if (m.model_type != "generative") continue;
        data.push_back({
            {"id",       m.id},
            {"object",   "model"},
            {"created",  static_cast<int64_t>(std::time(nullptr))},
            {"owned_by", ""},
        });
    }

    callback(jsonResp({
        {"object", "list"},
        {"data",   data},
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/models — OIP (all models)
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::listModelsV2(const HttpRequestPtr& /*req*/,
                                     std::function<void(const HttpResponsePtr&)>&& callback) {
    auto models = ModelConfigManager::getInstance().getAvailableModels();

    json arr = json::array();
    for (const auto& m : models) {
        json entry = {
            {"name",       m.id},
            {"model_type", m.model_type},
            {"runtime",    m.runtime},
        };
        if (m.model_type == "generative") {
            entry["supports_vision"]   = m.supports_vision;
            entry["supports_thinking"] = m.supports_thinking;
            entry["context_size"]      = m.context_size;
        }
        arr.push_back(entry);
    }

    callback(jsonResp(arr));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/models/{model_id} — OIP full metadata
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::getModelV2(const HttpRequestPtr& /*req*/,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& model_id) {
    const ModelConfig* m = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!m) {
        callback(jsonResp({{"error", "Model not found: " + model_id}}, k404NotFound));
        return;
    }

    json resp = {
        {"name",       m->id},
        {"model_type", m->model_type},
        {"runtime",    m->runtime},
        {"versions",   json::array({"1"})},
        {"platform",   "qualcomm_" + m->runtime},
    };

    if (m->model_type == "generative") {
        // Generative model: text I/O + optional image I/O
        resp["supports_vision"]   = m->supports_vision;
        resp["supports_thinking"] = m->supports_thinking;
        resp["supports_streaming"]= m->supports_streaming;
        resp["context_size"]      = m->context_size;
        resp["inputs"]            = buildGenerativeInputs(m->supports_vision);
        resp["outputs"]           = buildGenerativeOutputs();

        // Include chat_template so clients can format prompts themselves
        if (!m->chat_template.is_null() && !m->chat_template.empty()) {
            resp["chat_template"] = m->chat_template;
        }
        if (m->vision_preprocessing.has_value()) {
            resp["vision_preprocessing"] = m->vision_preprocessing.value();
        }
    } else {
        // Predictive model: tensor I/O from metadata.json model_files
        resp["inputs"]  = buildTensorSpecs(m->input_specs);
        resp["outputs"] = buildTensorSpecs(m->output_specs);
        resp["memory_mb"] = m->memory_requirement_mb;

        // Postprocessing plugins compatible with this model's output tensors
        json extensions = json::array();
        for (const auto* p : PostprocRegistry::getInstance().getCompatible(m->output_specs)) {
            extensions.push_back({
                {"name",        p->name},
                {"description", p->description},
                {"parameters",  buildParamsJson(p->parameters)},
            });
        }
        resp["extensions"] = extensions;
    }

    callback(jsonResp(resp));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/postprocesses — postprocessing plugins catalog
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::listPostprocesses(const HttpRequestPtr& /*req*/,
                                           std::function<void(const HttpResponsePtr&)>&& callback) {
    json arr = json::array();
    for (const auto* p : PostprocRegistry::getInstance().list()) {
        arr.push_back({
            {"name",        p->name},
            {"description", p->description},
            {"expects",     buildExpectsJson(p->layouts)},
            {"parameters",  buildParamsJson(p->parameters)},
        });
    }
    callback(jsonResp(arr));
}
