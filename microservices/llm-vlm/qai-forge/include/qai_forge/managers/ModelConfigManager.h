// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// ModelConfig — Pure data record for a single discovered model bundle
//
// This is a plain data struct. It holds no logic, no file handles, and no
// mutable state. It is populated by ModelConfigManager during bundle scanning
// and consumed by the orchestration pipeline (Layer 2) and controllers (Layer 1).
// ─────────────────────────────────────────────────────────────────────────────

struct ModelTensorSpec {
    std::string           name;
    std::vector<int64_t>  shape;
    std::string           dtype;  // "FP32", "INT8", etc.
};

struct ModelConfig {
    std::string id;                         // e.g. "qwen2.5-7b"
    std::string display_name;               // Human-readable name from metadata.json
    std::string config_file;                // Absolute path to the processed genie_config.json in /tmp/configs
    std::string sampler_config_file;        // Absolute path to the processed sampler config (if present)
    int context_size = 4096;                // Max context window in tokens (after capping)
    int memory_requirement_mb = 4096;       // Estimated memory footprint in MB
    bool supports_vision = false;           // True for VLM models
    bool supports_streaming = true;
    bool supports_thinking = false;         // True for reasoning models (e.g. DeepSeek-R1, Qwen3)
    std::string thinking_start_tag = "<think>";
    std::string thinking_end_tag = "</think>";
    int default_thinking_budget = 8192;
    json chat_template;                     // Chat template config from metadata.json
    std::optional<json> vision_preprocessing; // VLM-specific preprocessing params

    // Runtime identifier from metadata.json "runtime" field.
    // Used by BackendFactory to select the correct IGenerativeBackend.
    // Defaults to "genie" for backward compatibility with existing bundles
    // that already have "runtime": "genie" in their metadata.json.
    // Future values: "litert_lm", "onnxrt"
    std::string runtime = "genie";

    // Model type from metadata.json "model_type" field.
    // "generative"   — LLM/VLM models (routed to GenerativeOrchestrator)
    // "conventional" — classification/detection/segmentation (routed to ConventionalAIOrchestrator)
    // Defaults to "generative" for backward compatibility with existing bundles.
    std::string model_type = "generative";

    // Tensor specs for conventional AI models (from metadata.json)
    std::vector<ModelTensorSpec> input_specs;
    std::vector<ModelTensorSpec> output_specs;
};

// ─────────────────────────────────────────────────────────────────────────────
// ModelConfigManager — Layer 2 Singleton
//
// Responsibilities:
//   1. Scan the models directory at startup for model bundles.
//   2. Discover bundles via metadata.json (new format) or model_config.json (legacy).
//   3. Copy JSON config files to /tmp/configs to avoid corrupting original bundles.
//   4. Patch copied configs (context capping, sampler top-k, HTP polling).
//   5. Expose a clean, read-only API for the orchestration pipeline.
//
// Design decisions (from architecture_refactoring_design.md):
//   - This is a pure data manager. It does NOT spawn processes or hold handles.
//   - It is thread-safe via a shared_mutex (multiple readers, single writer on reload).
//   - The ModelAdapter pattern (Section 3.E) uses this manager to resolve
//     model-specific preprocessing and tool formatting adapters.
// ─────────────────────────────────────────────────────────────────────────────
class ModelConfigManager {
public:
    static ModelConfigManager& getInstance();

    // ── Discovery ─────────────────────────────────────────────────────────────

    /**
     * Scan the models directory and populate the internal model registry.
     * Called once at server startup (before any requests are served).
     * Can be called again to hot-reload if models are added/removed.
     */
    void scanModelBundles();

    // ── Query API (thread-safe, read-only) ────────────────────────────────────

    const ModelConfig* getModelConfig(const std::string& model_id) const;
    std::vector<ModelConfig> getAvailableModels() const;
    std::string getDefaultModelId() const;
    bool validateModel(const std::string& model_id) const;
    int getContextSize(const std::string& model_id) const;
    int getMemoryRequirementMb(const std::string& model_id) const;
    bool supportsVision(const std::string& model_id) const;
    bool supportsThinking(const std::string& model_id) const;
    std::string getConfigFilePath(const std::string& model_id) const;
    std::optional<json> getVisionPreprocessing(const std::string& model_id) const;
    json getChatTemplate(const std::string& model_id) const;

    // Returns the runtime identifier for a model ("genie", "litert_lm", "onnxrt").
    // Returns "genie" if the model is not found (safe default — existing behaviour).
    // Used by BackendFactory to select the correct IGenerativeBackend implementation.
    std::string getRuntime(const std::string& model_id) const;

    // Returns the model type ("generative" or "conventional").
    // Returns "generative" if the model is not found (safe default).
    // Used by InferenceRouter to select the correct orchestrator.
    std::string getModelType(const std::string& model_id) const;

private:
    ModelConfigManager() = default;
    ModelConfigManager(const ModelConfigManager&) = delete;
    ModelConfigManager& operator=(const ModelConfigManager&) = delete;

    // ── Internal bundle processing ─────────────────────────────────────────────

    /**
     * Process a single bundle directory:
     *   1. Copy all JSON files (except tokenizer.json) to /tmp/configs/<bundle_name>/
     *   2. Apply patches to the copies (context capping, sampler, HTP polling)
     *   3. Rewrite relative file paths to absolute paths
     * Returns the path to the processed config directory.
     */
    std::string processBundle(const std::string& bundle_path, const std::string& bundle_name);

    /**
     * Parse a metadata.json file into a ModelConfig.
     * Handles both LLM and VLM bundles.
     */
    ModelConfig parseMetadataJson(const json& metadata, const std::string& bundle_path,
                                  const std::string& processed_config_dir);

    /**
     * Generate a synthetic genie_config.json for VLM models.
     * Matches the structure expected by vlm-service.cpp.
     * Returns the path to the generated file.
     */
    std::string generateVlmGenieConfig(const json& metadata, const json& genie,
                                       const json& pipeline_nodes,
                                       const std::string& processed_config_dir);

    /**
     * Calculate memory requirement from .bin file sizes in the bundle.
     * Returns total_bytes * 1.25 in MB.
     */
    int calculateMemoryFromBinFiles(const std::string& bundle_path, const json& model_files) const;

    /**
     * Recursively rewrite relative file paths in a JSON object to absolute paths.
     * JSON files → point to the copy in processed_config_dir.
     * Binary/other files → point to the original bundle_path.
     * tokenizer.json → always points to the original bundle_path.
     */
    json rewritePaths(const json& data, const std::string& bundle_path,
                      const std::string& processed_config_dir) const;

    /**
     * Patch dialog.sampler.top-k: if top-k == 1, set to 2.
     * GenIE latches greedy mode at construction when top-k == 1, preventing
     * runtime sampling overrides from taking effect.
     */
    static void patchDialogSamplerConfig(json& data, const std::string& bundle_name,
                                         const std::string& filename);

    /**
     * Apply GENAI_CONTEXT_CAPPING env var to context.size fields.
     * Ensures the model never exceeds the configured context window cap.
     */
    static void applyContextCapping(json& data, const std::string& bundle_name,
                                    const std::string& filename);

    /**
     * Patch QnnHtp backend config to set poll: false.
     * Prevents worker threads from busy-waiting and consuming 100% CPU while idle.
     */
    static void patchHtpPollingConfig(json& data, const std::string& bundle_name,
                                      const std::string& filename);

    // ── State ──────────────────────────────────────────────────────────────────
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ModelConfig> models_;
    std::string default_model_id_;
    std::string models_dir_ = "/mnt/work/models";
    std::string tmp_config_dir_ = "/tmp/configs";
};
