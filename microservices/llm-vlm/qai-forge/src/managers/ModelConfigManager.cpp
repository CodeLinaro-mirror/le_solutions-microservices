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
#ifdef QAI_FORGE_BUILD_LLAMACPP
#include "qai_forge/utils/GgufMetadataReader.h"
#endif
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <set>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

std::string runtimeAliasKey(std::string runtime) {
    std::transform(runtime.begin(), runtime.end(), runtime.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    std::replace(runtime.begin(), runtime.end(), '-', '_');

    const std::string geniex_prefix = "geniex_";
    if (runtime.rfind(geniex_prefix, 0) == 0) {
        runtime.erase(0, geniex_prefix.size());
    }

    runtime.erase(std::remove(runtime.begin(), runtime.end(), '_'),
                  runtime.end());
    return runtime;
}

std::string normalizeRuntime(const std::string& runtime) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"genie", "genie"},
        {"qairt", "genie"},
        {"llama", "llamacpp"},
        {"llamacpp", "llamacpp"},
        {"litertlm", "litert_lm"},
    };

    const std::string key = runtimeAliasKey(runtime);
    auto it = aliases.find(key);
    return it != aliases.end() ? it->second : runtime;
}

}  // namespace

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
                // Model ID convention: "{model_id}-{runtime}-{precision}"
                // e.g. "qwen3_4b_instruct_2507-genie-w4a16", "nomic_embed_text-qnn_dlc-float"
                std::string runtime_str = metadata.value("runtime", "genie");
                std::string precision_str = metadata.value("precision", "float");
                config.id = model_id + "-" + runtime_str + "-" + precision_str;

                if (new_models.count(config.id)) {
                    LOG_WARN("[ModelConfigManager] Duplicate model id '" << config.id
                             << "' — skipping bundle " << bundle_name
                             << " (already loaded from a previous bundle)");
                    continue;
                }
                config.bundle_path = bundle_path;
                new_models[config.id] = std::move(config);

                if (new_default.empty()) new_default = config.id;
                LOG_INFO("[ModelConfigManager] Loaded model: " << config.id
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

                    if (new_models.count(model_id)) {
                        LOG_WARN("[ModelConfigManager] Duplicate model id '" << model_id
                                 << "' — skipping legacy entry in bundle " << bundle_name
                                 << " (already loaded from a previous bundle)");
                        continue;
                    }
                    config.bundle_path = bundle_path;
                    new_models[model_id] = std::move(config);
                    if (new_default.empty()) new_default = model_id;
                    LOG_INFO("[ModelConfigManager] Loaded legacy model: " << model_id);
                }

            } catch (const std::exception& e) {
                LOG_ERROR("[ModelConfigManager] Error processing legacy bundle "
                          << bundle_name << ": " << e.what());
            }
        }
        // ── GenieX format: geniex.json manifest ────────────────────────────
        // Written by the GenieX SDK (libgeniex.so) after a `geniex pull`.
        // AI-Hub-sourced pulls also carry a metadata.json (handled above);
        // this branch covers HuggingFace/GGUF pulls that ship only geniex.json.
        else if (fs::exists(bundle_path + "/geniex.json")) {
            try {
                std::ifstream f(bundle_path + "/geniex.json");
                json manifest = json::parse(f);

                auto configs = parseGenieXJson(manifest, bundle_path);
                for (auto& config : configs) {
                    if (config.id.empty()) {
                        LOG_WARN("[ModelConfigManager] Could not derive id from geniex.json: "
                                 << bundle_name);
                        continue;
                    }
                    std::string geniex_id = config.id;
                    if (new_models.count(geniex_id)) {
                        LOG_WARN("[ModelConfigManager] Duplicate model id '" << geniex_id
                                 << "' — skipping GenieX bundle " << bundle_name
                                 << " (already loaded from a previous bundle)");
                        continue;
                    }
                    config.bundle_path = bundle_path;
                    new_models[geniex_id] = std::move(config);
                    if (new_default.empty()) new_default = geniex_id;
                    LOG_INFO("[ModelConfigManager] Loaded GenieX model: "
                             << geniex_id << " from " << bundle_name);
                }

            } catch (const std::exception& e) {
                LOG_ERROR("[ModelConfigManager] Error processing GenieX bundle "
                          << bundle_name << ": " << e.what());
            }
        }
        // ── HF direct download: hf_manifest.json (geniex.json-compatible) ────
        // Written by HfDirectClient for repos with .task/.tflite/.litertlm files
        // that GenieX SDK cannot handle. Schema is identical to geniex.json so
        // parseGenieXJson can be reused directly.
        else if (fs::exists(bundle_path + "/hf_manifest.json")) {
            try {
                std::ifstream f(bundle_path + "/hf_manifest.json");
                json manifest = json::parse(f);

                auto configs = parseGenieXJson(manifest, bundle_path);
                for (auto& config : configs) {
                    if (config.id.empty()) {
                        LOG_WARN("[ModelConfigManager] Could not derive id from hf_manifest.json: "
                                 << bundle_name);
                        continue;
                    }
                    std::string hf_id = config.id;
                    if (new_models.count(hf_id)) {
                        LOG_WARN("[ModelConfigManager] Duplicate model id '" << hf_id
                                 << "' — skipping HF bundle " << bundle_name
                                 << " (already loaded from a previous bundle)");
                        continue;
                    }
                    config.bundle_path = bundle_path;
                    new_models[hf_id] = std::move(config);
                    if (new_default.empty()) new_default = hf_id;
                    LOG_INFO("[ModelConfigManager] Loaded HF direct model: "
                             << hf_id << " from " << bundle_name);
                }

            } catch (const std::exception& e) {
                LOG_ERROR("[ModelConfigManager] Error processing HF bundle "
                          << bundle_name << ": " << e.what());
            }
        }
    }

    // ── LiteRT-LM: discover bare .litertlm files ──────────────────────────────
    // .litertlm bundles don't ship with metadata.json — the model metadata
    // (Jinja template, context length, tool call delimiters) is embedded inside
    // the binary and extracted by the worker at load time via the METADATA IPC
    // event. We auto-generate a ModelConfig for each .litertlm file found.
    //
    // Discovery rules:
    //   - Scans models_dir_ recursively for files with extension ".litertlm"
    //   - Model ID: "{stem}-litert_lm"  (e.g. "gemma3-4b-it-litert_lm")
    //   - Skips files whose model_id was already registered via metadata.json
    //   - context_size defaults to 4096; actual value comes from the METADATA
    //     IPC event sent by the worker after loading the model file
    for (const auto& entry : fs::recursive_directory_iterator(models_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".litertlm") continue;

        const std::string file_path = entry.path().string();
        const std::string stem      = entry.path().stem().string();
        const std::string model_id  = stem + "-litert_lm";

        // Skip if already registered (e.g. via metadata.json in the same directory)
        if (new_models.count(model_id)) {
            LOG_DEBUG("[ModelConfigManager] Skipping .litertlm auto-discovery for '"
                      << model_id << "' — already registered via metadata.json");
            continue;
        }

        try {
            ModelConfig config;
            config.id                  = model_id;
            config.display_name        = stem;
            config.runtime             = "litert_lm";
            config.model_type          = "generative";
            config.config_file         = file_path;
            config.context_size        = 4096;  // actual value from METADATA IPC event
            config.supports_streaming  = true;
            config.supports_vision     = false;
            config.supports_thinking   = false;
            config.thinking_start_tag  = "<think>";
            config.thinking_end_tag    = "</think>";

            // Estimate memory from file size (×1.25 for runtime overhead)
            std::error_code ec;
            uintmax_t file_bytes = fs::file_size(entry.path(), ec);
            config.memory_requirement_mb = (!ec && file_bytes > 0)
                ? static_cast<int>(static_cast<double>(file_bytes) / (1024.0 * 1024.0) * 1.25)
                : 4096;

            config.bundle_path = entry.path().parent_path().string();
            new_models[model_id] = std::move(config);
            if (new_default.empty()) new_default = model_id;

            LOG_INFO("[ModelConfigManager] Auto-discovered LiteRT-LM model: "
                     << model_id << " -> " << file_path);

        } catch (const std::exception& e) {
            LOG_ERROR("[ModelConfigManager] Error auto-discovering .litertlm file "
                      << file_path << ": " << e.what());
        }
    }

    // ── llama.cpp: discover bare .gguf files ──────────────────────────────────
    // .gguf files contain embedded metadata (chat template, context length) in
    // their file header. GgufMetadataReader extracts this without loading weights.
    //
    // Discovery rules:
    //   - Scans models_dir_ recursively for files with extension ".gguf"
    //   - Model ID: "{stem}-llamacpp"  (e.g. "llama-3-8b-instruct-q4_0-llamacpp")
    //   - Skips files whose model_id was already registered via metadata.json
    //   - context_size and chat_template extracted from GGUF header
#ifdef QAI_FORGE_BUILD_LLAMACPP
    for (const auto& entry : fs::recursive_directory_iterator(models_dir_)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".gguf") continue;

        // Skip .gguf files whose parent directory already has a geniex.json or
        // metadata.json manifest — those bundles are registered by a higher-priority
        // branch and the individual .gguf files (including mmproj) must not be
        // registered again as separate models.
        const fs::path parent = entry.path().parent_path();
        if (fs::exists(parent / "geniex.json") || fs::exists(parent / "metadata.json")) {
            LOG_DEBUG("[ModelConfigManager] Skipping .gguf auto-discovery for '"
                      << entry.path().filename().string()
                      << "' — parent directory has a manifest (geniex.json/metadata.json)");
            continue;
        }

        const std::string file_path = entry.path().string();
        const std::string stem      = entry.path().stem().string();
        const std::string model_id  = stem + "-llamacpp";

        // Skip if already registered (e.g. via metadata.json in the same directory)
        if (new_models.count(model_id)) {
            LOG_DEBUG("[ModelConfigManager] Skipping .gguf auto-discovery for '"
                      << model_id << "' — already registered via metadata.json");
            continue;
        }

        try {
            auto meta = GgufMetadataReader::readMetadata(file_path);

            ModelConfig config;
            config.id                  = model_id;
            config.display_name        = stem;
            config.runtime             = "llamacpp";
            config.model_type          = "generative";
            config.config_file         = file_path;
            config.context_size        = meta.isValid ? meta.contextLength : 4096;

            // Apply context capping from environment
            const char* cap_env_gguf = std::getenv("GENAI_CONTEXT_CAPPING");
            if (cap_env_gguf) {
                try {
                    int cap = std::stoi(cap_env_gguf);
                    if (cap != -1) {
                        config.context_size = std::min(config.context_size, cap);
                    }
                } catch (...) {}
            }

            config.supports_streaming  = true;
            config.supports_vision     = false;
            config.supports_thinking   = false;
            config.thinking_start_tag  = "<think>";
            config.thinking_end_tag    = "</think>";

            // Store Jinja template from GGUF header
            if (meta.isValid && !meta.chatTemplate.empty()) {
                config.chat_template["jinja_template"] = meta.chatTemplate;
            }

            // Estimate memory from file size (×1.25 for runtime overhead)
            std::error_code ec;
            uintmax_t file_bytes = fs::file_size(entry.path(), ec);
            config.memory_requirement_mb = (!ec && file_bytes > 0)
                ? static_cast<int>(static_cast<double>(file_bytes) / (1024.0 * 1024.0) * 1.25)
                : 4096;

            config.bundle_path = entry.path().parent_path().string();
            new_models[model_id] = std::move(config);
            if (new_default.empty()) new_default = model_id;

            LOG_INFO("[ModelConfigManager] Auto-discovered llama.cpp model: "
                     << model_id << " -> " << file_path
                     << " (ctx=" << config.context_size << ")");

        } catch (const std::exception& e) {
            LOG_ERROR("[ModelConfigManager] Error auto-discovering .gguf file "
                      << file_path << ": " << e.what());
        }
    }
#endif // QAI_FORGE_BUILD_LLAMACPP

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
            if (cap != -1) {
                config.context_size = std::min(config.context_size, cap);
            }
        } catch (...) {}
    }

    // ── Model type ────────────────────────────────────────────────────────────
    // Read from metadata.json "model_type" field. Older/AI-Hub-published
    // bundles have no such field — infer it from "runtime" instead of
    // defaulting to "generative", so predictive backends (QNN/SNPE/LiteRT)
    // aren't misrouted to the GenerativeOrchestrator. Falls back to
    // "generative" only when runtime itself doesn't match a known predictive
    // spelling (covers "genie", "litert_lm", "onnxrt", etc).
    config.runtime = normalizeRuntime(metadata.value("runtime", "genie"));
    if (metadata.contains("model_type")) {
        config.model_type = metadata.value("model_type", "generative");
    } else {
        static const std::set<std::string> predictive_runtimes = {
            "qnn", "qnn_context_binary",
            "snpe", "qnn_dlc",
            "litert", "tflite",
        };
        config.model_type = predictive_runtimes.count(config.runtime) ? "predictive" : "generative";
    }

    auto pipeline_nodes = genie.value("pipeline", json::object()).value("nodes", json::object());

    if (config.model_type == "predictive") {
        auto model_files = metadata.value("model_files", json::object());
        std::string model_file;
        if (!model_files.empty()) {
            model_file = model_files.begin().key();
            config.config_file = bundle_path + "/" + fs::path(model_file).filename().string();
        }
    }
    else if (config.runtime == "litert_lm") {
        // Phase 5: LiteRT-LM model discovery
        // The config_file points to the .litertlm model bundle file.
        // Look for a .litertlm file in the bundle directory.
        auto model_files = metadata.value("model_files", json::object());
        if (!model_files.empty()) {
            // Use the first model file listed in metadata.json
            std::string model_file = model_files.begin().key();
            config.config_file = bundle_path + "/" + fs::path(model_file).filename().string();
        } else {
            // Scan the bundle directory for a .litertlm file
            for (const auto& entry : fs::directory_iterator(bundle_path)) {
                if (entry.path().extension() == ".litertlm") {
                    config.config_file = entry.path().string();
                    break;
                }
            }
        }
        LOG_DEBUG("[ModelConfigManager] LiteRT-LM model: " << config.config_file);
    }
    else if (config.supports_vision) {
        config.config_file = generateVlmGenieConfig(metadata, genie, pipeline_nodes, processed_config_dir);
    } else {
        std::string text_gen = pipeline_nodes.value("textGenerator", "");
        if (!text_gen.empty()) {
            config.config_file = processed_config_dir + "/" + fs::path(text_gen).filename().string();
        }
        if (config.config_file.empty()) {
            std::string fallback = processed_config_dir + "/genie_config.json";
            if (fs::exists(fallback)) {
                config.config_file = fallback;
            }
        }
    }

    config.memory_requirement_mb = calculateMemoryFromBinFiles(bundle_path, metadata.value("model_files", json::object()));
    config.chat_template = genie.value("chat_template", json::object());

    if (genie.contains("vision_preprocessing") && !genie["vision_preprocessing"].is_null()) {
        config.vision_preprocessing = genie["vision_preprocessing"];
    }

    // ── Tensor Specs (for Predictive AI) ────────────────────────────────────
    // Primary source: input_specs / output_specs (flat array format).
    // Fallback: model_files.<filename>.inputs / outputs (dict-of-dicts format,
    //   where each key is the tensor name and the value holds shape/dtype).
    // Only the first model file's specs are used (single-model bundles).
    auto normalizeDtype = [](const std::string& dt) -> std::string {
        // Normalize lowercase dtype strings (e.g. "uint8") to OIP uppercase ("UINT8")
        std::string upper;
        for (char c : dt) upper += static_cast<char>(std::toupper(c));

        // Some model bundles spell dtypes out in full (e.g. "FLOAT32") instead
        // of the KFServing v2 abbreviated form ("FP32") — canonicalize known
        // aliases so every consumer (OIP JSON, gRPC InferService, TensorDTOs)
        // sees a single valid dtype string.
        static const std::unordered_map<std::string, std::string> aliases = {
            {"FLOAT32", "FP32"},
            {"FLOAT16", "FP16"},
            {"FLOAT",   "FP32"},
        };
        auto it = aliases.find(upper);
        return it != aliases.end() ? it->second : upper;
    };

    // Reads quantization_parameters.{scale,zero_point} from a tensor-spec JSON
    // object (if present) into the given ModelTensorSpec. Defaults (1.0/0) are
    // left untouched for unquantized tensors.
    auto applyQuantParams = [](ModelTensorSpec& spec, const json& spec_json) {
        if (spec_json.contains("quantization_parameters") &&
            spec_json["quantization_parameters"].is_object()) {
            const auto& qp = spec_json["quantization_parameters"];
            spec.quant_scale      = qp.value("scale", 1.0f);
            spec.quant_zero_point = qp.value("zero_point", 0);
        }
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
            applyQuantParams(spec, spec_json);
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
                applyQuantParams(spec, tensor_info);
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
            applyQuantParams(spec, spec_json);
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
                applyQuantParams(spec, tensor_info);
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
// parseGenieXJson — Build a ModelConfig from a GenieX SDK manifest
//
// The geniex.json schema is written by the closed-source GenieX SDK. Its real
// keys are PascalCase (confirmed against an AI-Hub pull on-device):
//   {"Name":"qualcomm/Qwen3-4B-Instruct-2507","ModelName":"qwen3_4b_instruct_2507",
//    "ModelType":"llm","PluginId":"qairt","Precision":"W4A16",
//    "ModelFile":{...},"MMProjFile":{...},"ExtraFiles":[...]}
// We read the PascalCase keys first and keep snake_case fallbacks for forward
// compatibility. Note: AI-Hub pulls also ship metadata.json (handled by the
// earlier branch, which wins); this branch primarily serves HuggingFace/GGUF
// pulls that ship only geniex.json.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<ModelConfig> ModelConfigManager::parseGenieXJson(const json& manifest,
                                                const std::string& bundle_path) {
    ModelConfig config;

    // ── Runtime / plugin id ───────────────────────────────────────────────────
    std::string runtime = manifest.value("PluginId",
                          manifest.value("plugin_id",
                          manifest.value("runtime", std::string("qairt"))));
    config.runtime = normalizeRuntime(runtime);

    // ── Model name / id ───────────────────────────────────────────────────────
    // Prefer ModelName (architecture id, already filesystem-friendly); fall back
    // to Name ("org/repo"), then the on-disk bundle directory name.
    std::string bundle_name = fs::path(bundle_path).filename().string();
    std::string model_id = manifest.value("ModelName",
                           manifest.value("model_name",
                           manifest.value("Name",
                           manifest.value("name", bundle_name))));
    // Name may be "org/repo"; keep only the leaf.
    if (auto slash = model_id.find_last_of('/'); slash != std::string::npos) {
        model_id = model_id.substr(slash + 1);
    }

    config.display_name = manifest.value("display_name", model_id);

    // Include precision in the id so different quantizations of the same model
    // get distinct entries (e.g. "Qwen3-VL-4B-Instruct-Q4_K_M-llamacpp").
    // Priority: geniex.json "Precision" field → ModelFile first key (quant name)
    // → empty (fall back to plain model_id-runtime).
    std::string precision = manifest.value("Precision",
                            manifest.value("precision", std::string("")));
    if (precision.empty()) {
        // GenieX SDK does not write Precision into geniex.json; derive it from the
        // ModelFile object whose key is the quantization name (e.g. "Q4_K_M").
        // Only pick a key whose file is actually downloaded and present on disk.
        auto model_file = manifest.value("ModelFile", json::object());
        if (model_file.is_object() && !model_file.empty()) {
            for (const auto& [k, v] : model_file.items()) {
                if (!v.value("Downloaded", false)) continue;
                std::string fname = v.value("Name", "");
                if (fname.empty()) continue;
                if (fs::exists(fs::path(bundle_path) / fname)) {
                    precision = k;
                    break;
                }
            }
        }
    }
    // Normalise to lowercase for consistent id formatting.
    for (auto& c : precision) c = static_cast<char>(std::tolower(c));
    if (!precision.empty()) {
        config.id = model_id + "-" + precision + "-" + runtime;
    } else {
        config.id = model_id + "-" + runtime;
    }

    // ── Modality ──────────────────────────────────────────────────────────────
    // ModelType is "llm"/"vlm" (string). Also honor an int form (0=LLM,1=VLM)
    // and a presence of a non-empty MMProjFile as a VLM signal.
    bool is_vlm = false;
    const char* mt_key = manifest.contains("ModelType") ? "ModelType"
                       : (manifest.contains("model_type") ? "model_type" : nullptr);
    if (mt_key) {
        const auto& mt = manifest[mt_key];
        if (mt.is_string()) {
            std::string s = mt.get<std::string>();
            is_vlm = (s == "vlm" || s == "VLM");
        } else if (mt.is_number_integer()) {
            is_vlm = (mt.get<int>() == 1);  // geniex_ModelType: 0=LLM, 1=VLM
        }
    }
    // MMProjFile.Name non-empty ⇒ multimodal.
    if (manifest.contains("MMProjFile") && manifest["MMProjFile"].is_object()) {
        std::string mmproj = manifest["MMProjFile"].value("Name", "");
        if (!mmproj.empty()) is_vlm = true;
    }
    config.supports_vision = is_vlm;
    // GenieX models are generative (LLM/VLM), routed to the generative pipeline.
    config.model_type = "generative";
    config.supports_streaming = true;

    // ── Config / entry file ───────────────────────────────────────────────────
    // qairt bundles carry a Genie-style config. The manifest doesn't name it
    // directly, but ExtraFiles lists it; if genie_config.json exists in the
    // bundle, point at it so the backend can load it.
    fs::path genie_cfg = fs::path(bundle_path) / "genie_config.json";
    if (fs::exists(genie_cfg)) {
        config.config_file = genie_cfg.string();
    }

    // litert_lm bundles: use the first downloaded ModelFile (.task/.tflite/.litertlm)
    // as config_file. Each variant gets its own ModelConfig.
    if (config.config_file.empty() && config.runtime == "litert_lm") {
        auto model_file = manifest.value("ModelFile", json::object());
        if (model_file.is_object() && !model_file.empty()) {
            std::vector<ModelConfig> results;
            for (const auto& [quant_key, quant_val] : model_file.items()) {
                if (!quant_val.value("Downloaded", false)) continue;
                std::string fname = quant_val.value("Name", "");
                if (fname.empty()) continue;
                fs::path model_path = fs::path(bundle_path) / fname;
                if (!fs::exists(model_path)) continue;

                ModelConfig cfg = config;
                std::string prec = quant_key;
                for (auto& c : prec) c = static_cast<char>(std::tolower(c));
                {
                    std::string base_model_id = manifest.value("ModelName",
                                               manifest.value("Name", std::string("")));
                    if (auto slash = base_model_id.find_last_of('/'); slash != std::string::npos)
                        base_model_id = base_model_id.substr(slash + 1);
                    cfg.id = base_model_id + "-" + prec + "-" + config.runtime;
                }
                cfg.config_file = model_path.string();
                results.push_back(std::move(cfg));
            }
            if (!results.empty()) return results;
        }
    }

    // llama_cpp bundles: each downloaded quantization in ModelFile becomes a
    // separate ModelConfig with its own id and config_file path.
    if (config.config_file.empty() && config.runtime == "llamacpp") {
        auto model_file = manifest.value("ModelFile", json::object());
        if (model_file.is_object() && !model_file.empty()) {
            std::vector<ModelConfig> results;
            for (const auto& [quant_key, quant_val] : model_file.items()) {
                if (!quant_val.value("Downloaded", false)) continue;
                std::string fname = quant_val.value("Name", "");
                if (fname.empty()) continue;
                fs::path gguf_path = fs::path(bundle_path) / fname;
                if (!fs::exists(gguf_path)) continue;

                ModelConfig cfg = config;  // copy base config
                std::string prec = quant_key;
                for (auto& c : prec) c = static_cast<char>(std::tolower(c));
                {
                    std::string base_model_id = manifest.value("ModelName",
                                               manifest.value("Name", std::string("")));
                    if (auto slash = base_model_id.find_last_of('/'); slash != std::string::npos)
                        base_model_id = base_model_id.substr(slash + 1);
                    cfg.id = base_model_id + "-" + prec + "-" + config.runtime;
                }
                cfg.config_file = gguf_path.string();
                results.push_back(std::move(cfg));
            }
            if (!results.empty()) return results;
        }
    }

    return {config};
}


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
    if (cap == -1) return;

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
        try {
            int cap = std::stoi(cap_env);
            if (cap != -1) {
                size = std::min(size, cap);
            }
        } catch (...) {}
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

void ModelConfigManager::updateLiteRTLMMetadata(const std::string& model_id,
                                                 int max_context_length,
                                                 const std::string& jinja_template,
                                                 const std::string& tool_call_delimiter,
                                                 const std::string& tool_response_delimiter) {
    std::unique_lock lock(mutex_);
    auto it = models_.find(model_id);
    if (it == models_.end()) {
        LOG_WARN("[ModelConfigManager] updateLiteRTLMMetadata: model not found: " << model_id);
        return;
    }
    it->second.context_size = max_context_length;
    if (!jinja_template.empty()) {
        it->second.chat_template["jinja_template"] = jinja_template;
    }
    if (!tool_call_delimiter.empty()) {
        it->second.chat_template["tool_call_delimiter"] =
            tool_call_delimiter;
    }
    if (!tool_response_delimiter.empty()) {
        it->second.chat_template["tool_response_delimiter"] =
            tool_response_delimiter;
    }
    LOG_INFO("[ModelConfigManager] Updated LiteRT-LM metadata for " << model_id
             << ": ctx=" << max_context_length
             << " jinja=" << (jinja_template.empty() ? "(none)" : "(set)"));
}
