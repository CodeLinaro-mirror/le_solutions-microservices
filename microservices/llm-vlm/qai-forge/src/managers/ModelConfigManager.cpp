// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ModelConfigManager — Layer 2 Implementation
//
// Scans model bundles from the models directory, copies and patches JSON
// config files to /tmp/configs (to avoid corrupting original bundles), and
// exposes a clean read-only API to the orchestration pipeline.
//
// Adheres to architecture_refactoring_design.md:
//   - Pure data manager: no process spawning, no inference handles.
//   - Thread-safe via shared_mutex (multiple readers, single writer on reload).
//   - Supports both metadata.json (new format) and model_config.json (legacy).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
ModelConfigManager& ModelConfigManager::getInstance() {
    static ModelConfigManager instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// scanModelBundles — Entry point for bundle discovery
// ─────────────────────────────────────────────────────────────────────────────
void ModelConfigManager::scanModelBundles() {
    // Read models directory from environment variable
    const char* env_dir = std::getenv("GENAI_MODELS_DIR");
    if (env_dir) models_dir_ = env_dir;

    std::unordered_map<std::string, ModelConfig> new_models;
    std::string new_default;

    // Clean up and recreate /tmp/configs
    if (fs::exists(tmp_config_dir_)) {
        fs::remove_all(tmp_config_dir_);
    }
    fs::create_directories(tmp_config_dir_);

    if (!fs::exists(models_dir_)) {
        LOG_ERROR("[ModelConfigManager] Models directory not found: " << models_dir_);
        std::unique_lock lock(mutex_);
        models_ = std::move(new_models);
        default_model_id_ = new_default;
        return;
    }

    // Walk the models directory recursively
    for (const auto& entry : fs::recursive_directory_iterator(models_dir_)) {
        if (!entry.is_directory()) continue;

        const std::string bundle_path = entry.path().string();
        const std::string bundle_name = entry.path().filename().string();

        // ── New format: metadata.json ──────────────────────────────────────
        if (fs::exists(bundle_path + "/metadata.json")) {
            try {
                std::string processed_dir = processBundle(bundle_path, bundle_name);

                std::ifstream f(processed_dir + "/metadata.json");
                json metadata = json::parse(f);

                std::string model_id = metadata.value("model_id", "");
                if (model_id.empty()) {
                    LOG_WARN("[ModelConfigManager] No model_id in metadata.json: " << bundle_name);
                    continue;
                }

                ModelConfig config = parseMetadataJson(metadata, bundle_path, processed_dir);
                // Model ID convention: "{model_id}-{runtime}"
                // e.g. "qwen3_4b_instruct_2507-genie", "nomic_embed_text-qnn_dlc"
                std::string runtime_str = metadata.value("runtime", "genie");
                config.id = model_id + "-" + runtime_str;
                new_models[config.id] = std::move(config);

                if (new_default.empty()) new_default = model_id + "-" + runtime_str;
                LOG_INFO("[ModelConfigManager] Loaded model: " << model_id + "-" + runtime_str
                         << " from " << bundle_name);

            } catch (const std::exception& e) {
                LOG_ERROR("[ModelConfigManager] Error processing bundle "
                          << bundle_name << ": " << e.what());
            }
        }
        // ── Legacy format: model_config.json ──────────────────────────────
        else if (fs::exists(bundle_path + "/model_config.json")) {
            try {
                std::string processed_dir = processBundle(bundle_path, bundle_name);

                std::ifstream f(processed_dir + "/model_config.json");
                json model_config_data = json::parse(f);

                for (const auto& [model_id, model_info] : model_config_data.value("models", json::object()).items()) {
                    ModelConfig config;
                    config.id = model_id;
                    config.display_name = model_info.value("display_name", model_id);
                    config.context_size = model_info.value("max_tokens", 4096);
                    config.supports_streaming = model_info.value("supports_streaming", true);
                    config.supports_vision = model_info.value("supports_vision", false);
                    config.memory_requirement_mb = model_info.value("memory_requirement_mb", 4096);

                    // Resolve config_file to the processed copy
                    std::string orig_config = model_info.value("config_file", "");
                    if (!orig_config.empty()) {
                        config.config_file = processed_dir + "/" + fs::path(orig_config).filename().string();
                    }

                    new_models[model_id] = std::move(config);
                    if (new_default.empty()) new_default = model_id;
                    LOG_INFO("[ModelConfigManager] Loaded legacy model: " << model_id);
                }

            } catch (const std::exception& e) {
                LOG_ERROR("[ModelConfigManager] Error processing legacy bundle "
                          << bundle_name << ": " << e.what());
            }
        }
    }

    std::unique_lock lock(mutex_);
    models_ = std::move(new_models);
    default_model_id_ = new_default;
    LOG_INFO("[ModelConfigManager] Scan complete. " << models_.size() << " model(s) loaded.");
}

// ─────────────────────────────────────────────────────────────────────────────
// processBundle — Copy and patch JSON configs to /tmp/configs
// ─────────────────────────────────────────────────────────────────────────────
std::string ModelConfigManager::processBundle(const std::string& bundle_path, const std::string& bundle_name) {
    std::string output_dir = tmp_config_dir_ + "/" + bundle_name;
    fs::create_directories(output_dir);

    // Determine LLM config filename from metadata.json (for key normalization)
    std::string llm_config_filename;
    if (fs::exists(bundle_path + "/metadata.json")) {
        try {
            std::ifstream f(bundle_path + "/metadata.json");
            json meta = json::parse(f);
            auto genie = meta.value("genie", json::object());
            if (!genie.value("supports_vision", false)) {
                auto nodes = genie.value("pipeline", json::object()).value("nodes", json::object());
                llm_config_filename = nodes.value("textGenerator", "");
            }
        } catch (...) {}
    }

    // Process each JSON file in the bundle
    for (const auto& entry : fs::directory_iterator(bundle_path)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::string filename = entry.path().filename().string();

        // Never copy tokenizer.json — always reference the original
        if (filename == "tokenizer.json") continue;

        json data;
        try {
            std::ifstream f(entry.path());
            data = json::parse(f);
        } catch (...) {
            LOG_WARN("[ModelConfigManager] Failed to parse: " << entry.path().string());
            continue;
        }

        // Normalize LLM dialog config: replace non-"dialog" root key with "dialog"
        if (!llm_config_filename.empty() && filename == llm_config_filename &&
            !data.contains("dialog") && data.size() == 1) {
            std::string old_key = data.begin().key();
            data["dialog"] = data[old_key];
            data.erase(old_key);
            LOG_DEBUG("[ModelConfigManager] Normalized root key '" << old_key
                      << "' -> 'dialog' in " << bundle_name << "/" << filename);
        }

        // Apply patches to the copy (never to the original)
        patchDialogSamplerConfig(data, bundle_name, filename);
        applyContextCapping(data, bundle_name, filename);
        patchHtpPollingConfig(data, bundle_name, filename);

        // Rewrite relative paths to absolute paths
        data = rewritePaths(data, bundle_path, output_dir);

        // Write processed copy
        std::ofstream out(output_dir + "/" + filename);
        out << data.dump(4);
    }

    return output_dir;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseMetadataJson — Build a ModelConfig from metadata.json
// ─────────────────────────────────────────────────────────────────────────────
ModelConfig ModelConfigManager::parseMetadataJson(const json& metadata, const std::string& bundle_path,
                                                   const std::string& processed_config_dir) {
    ModelConfig config;
    config.display_name = metadata.value("model_name", metadata.value("model_id", "Unknown"));

    auto genie = metadata.value("genie", json::object());
    config.supports_vision = genie.value("supports_vision", false);
    config.supports_streaming = genie.value("supports_streaming", true);

    auto context_lengths = genie.value("context_lengths", json::array({4096}));
    config.context_size = context_lengths.empty() ? 4096 : context_lengths.back().get<int>();

    // Apply context capping from environment
    const char* cap_env = std::getenv("GENAI_CONTEXT_CAPPING");
    if (cap_env) {
        try {
            int cap = std::stoi(cap_env);
            config.context_size = std::min(config.context_size, cap);
        } catch (...) {}
    }

    // ── Model type ────────────────────────────────────────────────────────────
    // Read from metadata.json "model_type" field. Defaults to "generative" for
    // backward compatibility — all existing bundles are generative models.
    // "predictive" is used for classification/detection/segmentation models.
    config.model_type = metadata.value("model_type", "generative");

    auto pipeline_nodes = genie.value("pipeline", json::object()).value("nodes", json::object());

    if (config.model_type == "predictive") {
        auto model_files = metadata.value("model_files", json::object());
        std::string model_file;
        if (!model_files.empty()) {
            model_file = model_files.begin().key();
			config.config_file = bundle_path + "/" + fs::path(model_file).filename().string();
        }
    }
    else if (config.supports_vision) {
        config.config_file = generateVlmGenieConfig(metadata, genie, pipeline_nodes, processed_config_dir);
    } else {
        std::string text_gen = pipeline_nodes.value("textGenerator", "");
        if (!text_gen.empty()) {
            config.config_file = processed_config_dir + "/" + fs::path(text_gen).filename().string();
        }
    }

    config.memory_requirement_mb = calculateMemoryFromBinFiles(bundle_path, metadata.value("model_files", json::object()));
    config.chat_template = genie.value("chat_template", json::object());

    if (genie.contains("vision_preprocessing") && !genie["vision_preprocessing"].is_null()) {
        config.vision_preprocessing = genie["vision_preprocessing"];
    }

    // ── Runtime identifier ────────────────────────────────────────────────────
    // Read from metadata.json "runtime" field. Defaults to "genie" for backward
    // compatibility — all existing bundles already have "runtime": "genie".
    // Future values: "litert_lm", "onnxrt"
    // Used by BackendFactory to select the correct IGenerativeBackend.
    config.runtime = metadata.value("runtime", "genie");

    // ── Tensor Specs (for Predictive AI) ────────────────────────────────────
    // Primary source: input_specs / output_specs (flat array format).
    // Fallback: model_files.<filename>.inputs / outputs (dict-of-dicts format,
    //   where each key is the tensor name and the value holds shape/dtype).
    // Only the first model file's specs are used (single-model bundles).
    auto normalizeDtype = [](const std::string& dt) -> std::string {
        // Normalize lowercase dtype strings (e.g. "uint8") to OIP uppercase ("UINT8")
        std::string upper;
        for (char c : dt) upper += static_cast<char>(std::toupper(c));
        return upper;
    };

    if (metadata.contains("input_specs") && metadata["input_specs"].is_array()) {
        for (const auto& spec_json : metadata["input_specs"]) {
            ModelTensorSpec spec;
            spec.name  = spec_json.value("name", "");
            spec.dtype = normalizeDtype(spec_json.value("dtype", "FP32"));
            if (spec_json.contains("shape") && spec_json["shape"].is_array()) {
                for (const auto& d : spec_json["shape"])
                    spec.shape.push_back(d.get<int64_t>());
            }
            config.input_specs.push_back(spec);
        }
    } else if (metadata.contains("model_files") && metadata["model_files"].is_object()) {
        // Use the first model file entry
        const auto& first_file = metadata["model_files"].begin().value();
        if (first_file.contains("inputs") && first_file["inputs"].is_object()) {
            for (const auto& [tensor_name, tensor_info] : first_file["inputs"].items()) {
                ModelTensorSpec spec;
                spec.name  = tensor_name;
                spec.dtype = normalizeDtype(tensor_info.value("dtype", "FP32"));
                if (tensor_info.contains("shape") && tensor_info["shape"].is_array()) {
                    for (const auto& d : tensor_info["shape"])
                        spec.shape.push_back(d.get<int64_t>());
                }
                config.input_specs.push_back(spec);
            }
        }
    }

    if (metadata.contains("output_specs") && metadata["output_specs"].is_array()) {
        for (const auto& spec_json : metadata["output_specs"]) {
            ModelTensorSpec spec;
            spec.name  = spec_json.value("name", "");
            spec.dtype = normalizeDtype(spec_json.value("dtype", "FP32"));
            if (spec_json.contains("shape") && spec_json["shape"].is_array()) {
                for (const auto& d : spec_json["shape"])
                    spec.shape.push_back(d.get<int64_t>());
            }
            config.output_specs.push_back(spec);
        }
    } else if (metadata.contains("model_files") && metadata["model_files"].is_object()) {
        const auto& first_file = metadata["model_files"].begin().value();
        if (first_file.contains("outputs") && first_file["outputs"].is_object()) {
            for (const auto& [tensor_name, tensor_info] : first_file["outputs"].items()) {
                ModelTensorSpec spec;
                spec.name  = tensor_name;
                spec.dtype = normalizeDtype(tensor_info.value("dtype", "FP32"));
                if (tensor_info.contains("shape") && tensor_info["shape"].is_array()) {
                    for (const auto& d : tensor_info["shape"])
                        spec.shape.push_back(d.get<int64_t>());
                }
                config.output_specs.push_back(spec);
            }
        }
    }

    // ── Detect reasoning model capabilities ──────────────────────────────────
    // The ONLY source of truth is the explicit genie.supports_thinking flag
    // in metadata.json. Name-based auto-detection is intentionally not used —
    // it is fragile and can silently activate thinking on models that don't
    // support it, or fail to activate it on models with unusual names.
    // Bundle authors must explicitly set supports_thinking: true.
    config.supports_thinking = genie.value("supports_thinking", false);

    // ── Parse thinking tags ───────────────────────────────────────────────────
    // Thinking tags describe model OUTPUT parsing (not input formatting).
    // They belong at the genie level, not in chat_template.
    //
    // Fallback resolution order:
    //   1. genie.thinking.start_tag / genie.thinking.end_tag  (preferred)
    //   2. genie.chat_template.thinking_start / thinking_end  (legacy)
    //   3. Hardcoded defaults "<think>" / "</think>"
    if (genie.contains("thinking") && genie["thinking"].is_object()) {
        config.thinking_start_tag = genie["thinking"].value("start_tag", "<think>");
        config.thinking_end_tag   = genie["thinking"].value("end_tag",   "</think>");
        LOG_DEBUG("[ModelConfigManager] Loaded thinking tags from genie.thinking for "
                  << metadata.value("model_id", "?")
                  << ": start='" << config.thinking_start_tag
                  << "' end='" << config.thinking_end_tag << "'");
    } else {
        // Legacy: some bundles may have put them in chat_template
        auto ct = genie.value("chat_template", json::object());
        config.thinking_start_tag = ct.value("thinking_start", "<think>");
        config.thinking_end_tag   = ct.value("thinking_end",   "</think>");
    }

    return config;
}

// ─────────────────────────────────────────────────────────────────────────────
// generateVlmGenieConfig — Synthesize genie_config.json for VLM models
// ─────────────────────────────────────────────────────────────────────────────
std::string ModelConfigManager::generateVlmGenieConfig(const json& metadata, const json& genie,
                                                        const json& pipeline_nodes,
                                                        const std::string& processed_config_dir) {
    std::string model_name = metadata.value("model_name", "VLM");

    json pipeline_out = json::object();
    for (const auto& key : {"imageEncoder", "lutEncoder", "textGenerator"}) {
        if (pipeline_nodes.contains(key)) {
            pipeline_out[key] = pipeline_nodes[key];
        }
    }

    const std::set<std::string> DYNAMIC_TYPES = {
        "GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT",
        "GENIE_NODE_TEXT_ENCODER_TEXT_INPUT",
        "GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT"
    };

    json custom_inputs = json::array();
    for (const auto& sample : genie.value("sample_inputs", json::array())) {
        std::string input_type = sample.value("input_type", sample.value("node_io", ""));
        if (DYNAMIC_TYPES.count(input_type)) continue;

        std::string file_path = sample.value("file", "");
        if (!file_path.empty() && !fs::path(file_path).is_absolute()) {
            file_path = processed_config_dir + "/../" + file_path;
        }
        custom_inputs.push_back({
            {"node", sample.value("node", "")},
            {"input_type", input_type},
            {"file", file_path}
        });
    }

    auto chat_template = genie.value("chat_template", json::object());
    json model_entry = {
        {"description", model_name + " Vision-Language Model"},
        {"pipeline", {{"nodes", pipeline_out}}},
        {"custom_inputs", custom_inputs}
    };
    if (chat_template.contains("vision_start"))
        model_entry["vision_start_token"] = chat_template["vision_start"];
    if (chat_template.contains("vision_end"))
        model_entry["vision_end_token"] = chat_template["vision_end"];

    json genie_config = {{model_name, model_entry}};

    std::string output_path = processed_config_dir + "/generated_genie_config.json";
    std::ofstream out(output_path);
    out << genie_config.dump(2);
    LOG_DEBUG("[ModelConfigManager] Generated VLM genie_config: " << output_path);
    return output_path;
}

// ─────────────────────────────────────────────────────────────────────────────
// calculateMemoryFromBinFiles
// ─────────────────────────────────────────────────────────────────────────────
int ModelConfigManager::calculateMemoryFromBinFiles(const std::string& bundle_path, const json& model_files) const {
    uintmax_t total_bytes = 0;

    if (!model_files.empty()) {
        for (const auto& [filename, _] : model_files.items()) {
            fs::path p = fs::path(bundle_path) / filename;
            if (fs::is_regular_file(p)) total_bytes += fs::file_size(p);
        }
    } else {
        for (const auto& entry : fs::directory_iterator(bundle_path)) {
            if (entry.path().extension() == ".bin") {
                total_bytes += fs::file_size(entry.path());
            }
        }
    }

    if (total_bytes == 0) return 4096;
    return static_cast<int>(static_cast<double>(total_bytes) / (1024.0 * 1024.0) * 1.25);
}

// ─────────────────────────────────────────────────────────────────────────────
// rewritePaths — Recursively rewrite relative paths to absolute paths
// ─────────────────────────────────────────────────────────────────────────────
json ModelConfigManager::rewritePaths(const json& data, const std::string& bundle_path,
                                       const std::string& processed_config_dir) const {
    if (data.is_object()) {
        json result = json::object();
        for (const auto& [k, v] : data.items()) {
            result[k] = rewritePaths(v, bundle_path, processed_config_dir);
        }
        return result;
    } else if (data.is_array()) {
        json result = json::array();
        for (const auto& item : data) {
            result.push_back(rewritePaths(item, bundle_path, processed_config_dir));
        }
        return result;
    } else if (data.is_string()) {
        std::string val = data.get<std::string>();

        // Skip strings that are clearly not file paths:
        //   - Longer than 255 chars (OS filename component limit)
        //   - Contain newlines (Jinja templates, multi-line text)
        //   - Contain spaces (config values, descriptions)
        // This prevents ENAMETOOLONG filesystem errors when metadata.json
        // contains large chat_template Jinja strings.
        if (val.size() > 255 ||
            val.find('\n') != std::string::npos ||
            val.find(' ')  != std::string::npos) {
            return data;
        }

        // Use the non-throwing is_regular_file overload so that any remaining
        // edge-case invalid paths (e.g. containing special characters) are
        // silently skipped rather than aborting the bundle load.
        std::error_code ec;
        fs::path potential = fs::path(bundle_path) / val;
        if (fs::is_regular_file(potential, ec) && !ec) {
            std::string filename = potential.filename().string();
            if (filename == "tokenizer.json") return potential.string();
            if (potential.extension() == ".json") return processed_config_dir + "/" + filename;
            return potential.string();
        }
        return data;
    }
    return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// patchDialogSamplerConfig
// ─────────────────────────────────────────────────────────────────────────────
void ModelConfigManager::patchDialogSamplerConfig(json& data, const std::string& bundle_name,
                                                   const std::string& filename) {
    auto patch_sampler = [&](json& node, const std::string& node_key) {
        if (!node.is_object()) return;
        auto it = node.find("sampler");
        if (it == node.end() || !it->is_object()) return;
        auto& sampler = *it;
        if (sampler.value("top-k", 0) == 1) {
            sampler["top-k"] = 2;
            LOG_DEBUG("[ModelConfigManager] Patched " << node_key
                      << ".sampler.top-k 1->2 in " << bundle_name << "/" << filename);
        }
    };

    for (const auto& key : {"dialog", "text-generator", "text_generator", "textGenerator"}) {
        if (data.contains(key)) patch_sampler(data[key], key);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// applyContextCapping
// ─────────────────────────────────────────────────────────────────────────────
void ModelConfigManager::applyContextCapping(json& data, const std::string& bundle_name,
                                              const std::string& filename) {
    const char* cap_env = std::getenv("GENAI_CONTEXT_CAPPING");
    if (!cap_env) return;
    int cap;
    try { cap = std::stoi(cap_env); } catch (...) { return; }

    auto cap_node = [&](json& node, const std::string& node_key) {
        if (!node.is_object()) return;
        auto ctx_it = node.find("context");
        if (ctx_it == node.end() || !ctx_it->is_object()) return;
        auto& ctx = *ctx_it;
        if (ctx.contains("size") && ctx["size"].is_number_integer()) {
            int orig = ctx["size"].get<int>();
            if (orig > cap) {
                ctx["size"] = cap;
                LOG_DEBUG("[ModelConfigManager] Context capped " << node_key
                          << ".context.size " << orig << "->" << cap
                          << " in " << bundle_name << "/" << filename);
            }
        }
    };

    for (const auto& key : {"dialog", "text-generator", "text_generator", "textGenerator"}) {
        if (data.contains(key)) cap_node(data[key], key);
    }
    if (data.contains("context")) cap_node(data, "root");
}

// ─────────────────────────────────────────────────────────────────────────────
// patchHtpPollingConfig
// ─────────────────────────────────────────────────────────────────────────────
void ModelConfigManager::patchHtpPollingConfig(json& data, const std::string& bundle_name,
                                                const std::string& filename) {
    const std::vector<std::string> node_keys = {
        "dialog", "text-generator", "text_generator", "textGenerator",
        "image-encoder", "imageEncoder", "lutEncoder", "text-encoder"
    };
    for (const auto& node_key : node_keys) {
        if (!data.contains(node_key) || !data[node_key].is_object()) continue;
        auto& node = data[node_key];
        if (!node.contains("engine") || !node["engine"].is_object()) continue;
        auto& engine = node["engine"];
        if (!engine.contains("backend") || !engine["backend"].is_object()) continue;
        auto& backend = engine["backend"];
        for (const auto& htp_key : {"QnnHtp", "qnnHtp"}) {
            if (backend.contains(htp_key) && backend[htp_key].is_object()) {
                if (backend[htp_key].value("poll", false) == true) {
                    backend[htp_key]["poll"] = false;
                    LOG_DEBUG("[ModelConfigManager] Patched QnnHtp poll=false in "
                              << bundle_name << "/" << filename << " (" << node_key << ")");
                }
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Query API
// ─────────────────────────────────────────────────────────────────────────────
const ModelConfig* ModelConfigManager::getModelConfig(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) ? &it->second : nullptr;
}

std::vector<ModelConfig> ModelConfigManager::getAvailableModels() const {
    std::shared_lock lock(mutex_);
    std::vector<ModelConfig> result;
    result.reserve(models_.size());
    for (const auto& [id, cfg] : models_) result.push_back(cfg);
    return result;
}

std::string ModelConfigManager::getDefaultModelId() const {
    std::shared_lock lock(mutex_);
    return default_model_id_;
}

bool ModelConfigManager::validateModel(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    return models_.count(model_id) > 0;
}

int ModelConfigManager::getContextSize(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    int size = (it != models_.end()) ? it->second.context_size : 4096;
    const char* cap_env = std::getenv("GENAI_CONTEXT_CAPPING");
    if (cap_env) {
        try { size = std::min(size, std::stoi(cap_env)); } catch (...) {}
    }
    return size;
}

int ModelConfigManager::getMemoryRequirementMb(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) ? it->second.memory_requirement_mb : 4096;
}

bool ModelConfigManager::supportsVision(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) && it->second.supports_vision;
}

bool ModelConfigManager::supportsThinking(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) && it->second.supports_thinking;
}

std::string ModelConfigManager::getConfigFilePath(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) ? it->second.config_file : "";
}

std::optional<json> ModelConfigManager::getVisionPreprocessing(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    if (it != models_.end()) return it->second.vision_preprocessing;
    return std::nullopt;
}

json ModelConfigManager::getChatTemplate(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    return (it != models_.end()) ? it->second.chat_template : json::object();
}

std::string ModelConfigManager::getRuntime(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    // Default to "genie" — safe for all existing bundles
    return (it != models_.end()) ? it->second.runtime : "genie";
}

std::string ModelConfigManager::getModelType(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    auto it = models_.find(model_id);
    // Default to "generative" — safe for all existing bundles
    return (it != models_.end()) ? it->second.model_type : "generative";
}
