//===========================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//===========================================================================

#include "vlm-service.hpp"
#include "vlm-interface.h"
#include <iostream> // Explicitly included for std::cout
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <json/json.h>
#include <cstddef> // Explicitly included for size_t
#include <thread>   // For std::this_thread::sleep_for
#include <chrono>   // For std::chrono::seconds
#include <cstdio>   // For std::vfprintf
#include <cstdlib>  // For std::getenv
#include <cstdarg>  // For va_list, vsnprintf

static void customGenieLogCallback(
    const _GenieLog_Handle_t* handle,
    const char* format,
    GenieLog_Level_t level,
    long unsigned int timestampOrSize,
    va_list args) {
    (void)handle;
    (void)timestampOrSize;

    const char* levelStr = "INFO";
    if (level == GENIE_LOG_LEVEL_ERROR) {
        levelStr = "ERROR";
    } else if (level == GENIE_LOG_LEVEL_WARN) {
        levelStr = "WARN";
    } else if (level == GENIE_LOG_LEVEL_VERBOSE) {
        levelStr = "VERBOSE";
    }

    char buffer[4096];
    if (format) {
        vsnprintf(buffer, sizeof(buffer), format, args);
    } else {
        buffer[0] = '\0';
    }

    std::fprintf(stdout, "[GenIE-SDK] [%s] %s\n", levelStr, buffer);
    std::fflush(stdout);
}

Log::Log(GenieLog_Level_t logLevel) {
    const int32_t status = GenieLog_create(
        nullptr, customGenieLogCallback, logLevel, &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error(
            "Failed to create the log handle.");
    }
}

Log::~Log() {
    const int32_t status = GenieLog_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the log handle." << std::endl;
    }
}

void SamplerConfig::createSamplerConfig(
    const std::string& configPath) {
    std::ifstream confStream(configPath);
    std::string config;
    std::getline(confStream, config, '\0');
    m_config = config;
    const int32_t status = GenieSamplerConfig_createFromJson(
        config.c_str(), &m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to create sampler config.");
    }
}

void SamplerConfig::setParam(
    const std::string& keyStr,
    const std::string& valueStr) {
    const int32_t status = GenieSamplerConfig_setParam(
        m_handle, keyStr.c_str(), valueStr.c_str());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to setParam");
    }
}

SamplerConfig::~SamplerConfig() {
    const int32_t status = GenieSamplerConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the sampler config."
                  << std::endl;
    }
}

/*--------------------------------------------------------------------
 * Helper: convert string to GenieNode_IOName_t
 *--------------------------------------------------------------------*/
static GenieNode_IOName_t stringToNodeIO(
    const std::string& nodeIOString) {
    static const std::unordered_map<std::string, GenieNode_IOName_t>
        nodeIOMap = {
            {"GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT",
             GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT},
            {"GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT",
             GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT},
            {"GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT",
             GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT},
            {"GENIE_NODE_TEXT_ENCODER_TEXT_INPUT",
             GENIE_NODE_TEXT_ENCODER_TEXT_INPUT},
            {"GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT",
             GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT},
            {"GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT",
             GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT},
            {"GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT",
             GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT},
            {"GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN",
             GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN},
            {"GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS",
             GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS},
            {"GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK",
             GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK},
            {"GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK",
             GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK}
    };
    auto it = nodeIOMap.find(nodeIOString);
    if (it != nodeIOMap.end()) {
        return it->second;
    }
    throw std::invalid_argument(
        "Invalid Node IO value passed: " + nodeIOString);
}

static GenieLog_Level_t parseGenieLogLevel(const char* levelStr) {
    if (!levelStr) {
        return GENIE_LOG_LEVEL_INFO;
    }
    std::string level(levelStr);
    std::transform(level.begin(), level.end(), level.begin(), ::tolower);
    if (level == "error" || level == "err") {
        return GENIE_LOG_LEVEL_ERROR;
    }
    if (level == "warn" || level == "warning") {
        return GENIE_LOG_LEVEL_WARN;
    }
    if (level == "info") {
        return GENIE_LOG_LEVEL_INFO;
    }
    if (level == "verbose" || level == "debug") {
        return GENIE_LOG_LEVEL_VERBOSE;
    }
    return GENIE_LOG_LEVEL_INFO;
}

/*--------------------------------------------------------------------
 * Pipeline::Config implementation
 *--------------------------------------------------------------------*/
Pipeline::Config::Config(
    const std::string& jsonConfig,
    std::shared_ptr<Log> log) : m_handle(nullptr) {
    const Genie_Status_t status =
        GeniePipelineConfig_createFromJson(
            jsonConfig.c_str(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error(
            "Failed to create the pipeline config");
    }
    if (log) {
        const Genie_Status_t bindStatus =
            GeniePipelineConfig_bindLogger(m_handle, (*log)());
        if (GENIE_STATUS_SUCCESS != bindStatus) {
            throw std::runtime_error(
                "Failed to bind the log handle with the "
                "pipeline config");
        }
    }
}

Pipeline::Config::~Config() {
    const Genie_Status_t status = GeniePipelineConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the pipeline config."
                  << std::endl;
    }
}

Pipeline::Config::Config(Config&& other) noexcept
    : m_handle(nullptr) {
    *this = std::move(other);
}

Pipeline::Config& Pipeline::Config::operator=(Config&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

/*--------------------------------------------------------------------
 * Pipeline implementation
 *--------------------------------------------------------------------*/
Pipeline::~Pipeline() {
    const Genie_Status_t status = GeniePipeline_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the pipeline." << std::endl;
    }
}

Pipeline::Pipeline(Pipeline&& other) noexcept : m_handle(nullptr) {
    *this = std::move(other);
}

Pipeline& Pipeline::operator=(Pipeline&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

inline void Pipeline::addNode(std::shared_ptr<Node> node) {
    const Genie_Status_t status = GeniePipeline_addNode(
        m_handle, (*node)());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to add node");
    }
}

inline void Pipeline::connect(
    std::shared_ptr<Node> producerNode,
    GenieNode_IOName_t producerIO,
    std::shared_ptr<Node> consumerNode,
    GenieNode_IOName_t consumerIO) {
    const Genie_Status_t status = GeniePipeline_connect(
        m_handle, (*producerNode)(), producerIO,
        (*consumerNode)(), consumerIO);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to connect");
    }
}

inline void Pipeline::execute(void* userData) {
    // Non-blocking call: executes asynchronously in the C++
    // background thread. It returns immediately. The lifetime of
    // 'userData' must be managed by the caller (e.g., by waiting
    // on a condition variable until all callbacks are completed).
    const Genie_Status_t status = GeniePipeline_execute(
        m_handle, userData);
    if (GENIE_STATUS_SUCCESS != status &&
        status != GENIE_STATUS_WARNING_CONTEXT_EXCEEDED) {
        throw std::runtime_error("Failed to execute");
    }
}

inline void Pipeline::reset() {
    const Genie_Status_t status = GeniePipeline_reset(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to reset");
    }
}

/*--------------------------------------------------------------------
 * Node::Config implementation
 *--------------------------------------------------------------------*/
Node::Config::Config(
    const std::string& jsonConfig,
    std::shared_ptr<Log> log) : m_handle(nullptr) {
    const Genie_Status_t status = GenieNodeConfig_createFromJson(
        jsonConfig.c_str(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error(
            "Failed to create the node config");
    }
    if (log) {
        const Genie_Status_t bindStatus =
            GenieNodeConfig_bindLogger(m_handle, (*log)());
        if (GENIE_STATUS_SUCCESS != bindStatus) {
            throw std::runtime_error(
                "Failed to bind the log handle with the "
                "node config");
        }
    }
}

Node::Config::~Config() {
    const Genie_Status_t status = GenieNodeConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the node config." << std::endl;
    }
}

Node::Config::Config(Config&& other) noexcept
    : m_handle(nullptr) {
    *this = std::move(other);
}

Node::Config& Node::Config::operator=(Config&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

/*--------------------------------------------------------------------
 * Node implementation
 *--------------------------------------------------------------------*/
Node::~Node() {
    const Genie_Status_t status = GenieNode_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cout << "Failed to free the Genie Node." << std::endl;
    }
}

Node::Node(Node&& other) noexcept : m_handle(nullptr) {
    *this = std::move(other);
}

Node& Node::operator=(Node&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

void Node::setData(
    GenieNode_IOName_t ioName,
    const std::string& text,
    const char* dataConfig) {
    const Genie_Status_t status = GenieNode_setData(
        m_handle, ioName, (void*)(text.c_str()),
        text.size(), dataConfig);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to set the text input data");
    }
}

void Node::setData(
    GenieNode_IOName_t ioName,
    const void* data,
    const size_t dataSize,
    const char* dataConfig) {
    const Genie_Status_t status = GenieNode_setData(
        m_handle, ioName, (void*)data, dataSize, dataConfig);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to set the embedding input data");
    }
}

void Node::setTextCallback(
    GenieNode_IOName_t ioName,
    GenieNode_TextOutput_Callback_t callback) {
    const Genie_Status_t status = GenieNode_setTextCallback(
        m_handle, ioName, callback);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to set the text output callback");
    }
}

void Node::setEmbeddingCallback(
    GenieNode_IOName_t ioName,
    GenieNode_EmbeddingOutputCallback_t callback) {
    const Genie_Status_t status = GenieNode_setEmbeddingCallback(
        m_handle, ioName, callback);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to set the embedding output callback");
    }
}

void Node::getSampler() {
    const int32_t status = GenieNode_getSampler(
        m_handle, &m_samplerHandle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to get sampler.");
    }
}

void Node::applyConfig(
    GenieSamplerConfig_Handle_t samplerConfigHandle) {
    const int32_t status = GenieSampler_applyConfig(
        m_samplerHandle, samplerConfigHandle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error(
            "Failed to apply sampler config.");
    }
}

/*--------------------------------------------------------------------
 * VLMObject implementation
 *--------------------------------------------------------------------*/
VLMObject::VLMObject(
    const std::string& model,
    const std::string& config_path,
    const std::string& sampler_config_path,
    bool streaming) {
    stream = streaming;
    query = std::make_unique<Query>();
    strlcpy(modelSelected, model.c_str(), sizeof(modelSelected));

    // Load configuration from JSON file if provided, otherwise
    // use hard-coded defaults
    if (!config_path.empty()) {
        loadConfig(config_path);
    } else {
        std::string modelName = model.empty() ? "Qwen2.5-VL-3B" : model;
        loadConfig(modelName);
    }

    const char* logLevelEnv = std::getenv("LOG_LEVEL");
    GenieLog_Level_t logLevel = parseGenieLogLevel(logLevelEnv);
    logger = std::make_shared<Log>(logLevel);
    std::cout << "[VLMObject] Genie log level: "
              << (logLevel == GENIE_LOG_LEVEL_ERROR ? "ERROR" :
                  logLevel == GENIE_LOG_LEVEL_WARN ? "WARN" :
                  logLevel == GENIE_LOG_LEVEL_INFO ? "INFO" :
                  "VERBOSE")
              << std::endl;

    // Build pipeline and nodes
    createPipelineAndNodes();

    // Connect nodes
    connectNodes();

    // Load static custom inputs (position ids, masks, etc.)
    loadStaticCustomInputs();

    sc_configPath = sampler_config_path.empty() ?
        "sampler.json" : sampler_config_path;
}

VLMObject::~VLMObject() {
    // All smart pointers clean up automatically
}

/*--------------------------------------------------------------------
 * Load model configuration from JSON file or use hard-coded
 * defaults
 *--------------------------------------------------------------------*/
void VLMObject::loadConfig(
    const std::string& configPathOrModelName) {
    // Check if this is a file path (contains .json) or a model
    // name
    if (configPathOrModelName.find(".json") !=
        std::string::npos) {
        // Load from JSON file
        std::ifstream configFile(configPathOrModelName);
        if (!configFile.is_open()) {
            throw std::runtime_error(
                "Failed to open config file: " +
                configPathOrModelName);
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;

        if (!Json::parseFromStream(
            builder, configFile, &root, &errs)) {
            throw std::runtime_error(
                "Failed to parse JSON config: " + errs);
        }

        // Get the first (and only) model configuration
        if (root.empty()) {
            throw std::runtime_error(
                "Empty JSON configuration file");
        }

        // Get the first model key
        std::string modelKey = root.getMemberNames()[0];
        const Json::Value& modelConfig_json = root[modelKey];

        // Validate required fields
        if (!modelConfig_json.isMember("pipeline") ||
            !modelConfig_json["pipeline"].isMember("nodes")) {
            throw std::runtime_error(
                "Invalid JSON structure: missing "
                "'pipeline.nodes'");
        }

        const Json::Value& nodes =
            modelConfig_json["pipeline"]["nodes"];

        // Extract node configuration paths
        if (!nodes.isMember("imageEncoder") ||
            !nodes.isMember("lutEncoder") ||
            !nodes.isMember("textGenerator")) {
            throw std::runtime_error(
                "Invalid JSON structure: missing required "
                "node configurations");
        }

        modelConfig.imageEncoderConfig =
            nodes["imageEncoder"].asString();
        modelConfig.lutEncoderConfig =
            nodes["lutEncoder"].asString();
        modelConfig.textGeneratorConfig =
            nodes["textGenerator"].asString();

        // Parse custom_inputs array
        modelConfig.custom_inputs.clear();
        if (modelConfig_json.isMember("custom_inputs") &&
            modelConfig_json["custom_inputs"].isArray()) {
            const Json::Value& customInputs =
                modelConfig_json["custom_inputs"];
            for (const auto& input : customInputs) {
                if (!input.isMember("node") ||
                    !input.isMember("input_type") ||
                    !input.isMember("file")) {
                    throw std::runtime_error(
                        "Invalid custom_input entry: missing "
                        "required fields");
                }

                ModelConfig::CustomInput ci;
                ci.node = input["node"].asString();
                ci.input_type = input["input_type"].asString();
                ci.file = input["file"].asString();
                modelConfig.custom_inputs.push_back(ci);
            }
        }

        // Optionally load vision token markers (defaults are set in ModelConfig)
        if (modelConfig_json.isMember("vision_start_token")) {
            modelConfig.visionStartToken = modelConfig_json["vision_start_token"].asString();
        }
        if (modelConfig_json.isMember("vision_end_token")) {
            modelConfig.visionEndToken = modelConfig_json["vision_end_token"].asString();
        }
    } else {
        // Use hard-coded configuration for backward compatibility
        if (configPathOrModelName != "QWEN2_5_VL_3B" &&
            configPathOrModelName != "Qwen2.5-VL-3B") {
            throw std::invalid_argument(
                "Unsupported VLM model: " +
                configPathOrModelName);
        }

        // Populate modelConfig with hard-coded values
        modelConfig.imageEncoderConfig = "qwen_veg.json";
        modelConfig.lutEncoderConfig = "text-encoder.json";
        modelConfig.textGeneratorConfig = "qwen-htp.json";

        // Custom static inputs for the image encoder
        modelConfig.custom_inputs = {
            {"imageEncoder",
             "GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS",
             "position_ids_cos.raw"},
            {"imageEncoder",
             "GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN",
             "position_ids_sin.raw"}
        };
    }
}

/*--------------------------------------------------------------------
 * Create pipeline and node objects
 *--------------------------------------------------------------------*/
void VLMObject::createPipelineAndNodes() {
    // Pipeline config (empty JSON string – we only need a handle
    // to bind profiler)
    auto pipelineCfg = std::make_shared<Pipeline::Config>(
        "", logger);
    pipeline = std::make_shared<Pipeline>(std::move(*pipelineCfg));

    // Create image encoder node
    std::string imgCfgStr;
    {
        std::ifstream f(modelConfig.imageEncoderConfig);
        if (!f) {
            throw std::runtime_error(
                "Failed to open " +
                modelConfig.imageEncoderConfig);
        }
        std::getline(f, imgCfgStr, '\0');
    }
    auto imgNodeCfg = std::make_shared<Node::Config>(
        imgCfgStr, logger);
    imageEncoderNode = std::make_shared<Node>(std::move(*imgNodeCfg));
    pipeline->addNode(imageEncoderNode);
    std::cout << "Created image encoder node" << std::endl;

    // LUT encoder node
    std::string lutCfgStr;
    {
        std::ifstream f(modelConfig.lutEncoderConfig);
        if (!f) {
            throw std::runtime_error(
                "Failed to open " +
                modelConfig.lutEncoderConfig);
        }
        std::getline(f, lutCfgStr, '\0');
    }
    auto lutNodeCfg = std::make_shared<Node::Config>(
        lutCfgStr, logger);
    lutEncoderNode = std::make_shared<Node>(std::move(*lutNodeCfg));
    pipeline->addNode(lutEncoderNode);

    // Create text generator node
    std::string txtGenCfgStr;
    {
        std::ifstream f(modelConfig.textGeneratorConfig);
        if (!f) {
            throw std::runtime_error(
                "Failed to open " +
                modelConfig.textGeneratorConfig);
        }
        std::getline(f, txtGenCfgStr, '\0');
    }
    auto txtGenNodeCfg = std::make_shared<Node::Config>(
        txtGenCfgStr, logger);
    textGeneratorNode = std::make_shared<Node>(std::move(*txtGenNodeCfg));
    pipeline->addNode(textGeneratorNode);
    std::cout << "Created text generator node" << std::endl;
}

/*--------------------------------------------------------------------
 * Connect the three nodes inside the pipeline
 *--------------------------------------------------------------------*/
void VLMObject::connectNodes() {
    // imageEncoder → textGenerator (embedding input)
    pipeline->connect(imageEncoderNode,
                      GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT,
                      textGeneratorNode,
                      GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);

    // lutEncoder → textGenerator (embedding input)
    pipeline->connect(lutEncoderNode,
                      GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT,
                      textGeneratorNode,
                      GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);

    // Register text output callback on the text generator node
    textGeneratorNode->setTextCallback(
        GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT,
        textOutputCallback);
}

/*--------------------------------------------------------------------
 * Load static custom inputs (position ids, masks, etc.)
 *--------------------------------------------------------------------*/
void VLMObject::loadStaticCustomInputs() {
    std::cout << "[VLMObject::loadStaticCustomInputs] "
              << "Loading custom inputs..." << std::endl;

    bool needs_allocation = currentStaticBuffers.empty();

    for (size_t i = 0; i < modelConfig.custom_inputs.size(); ++i) {
        const auto& ci = modelConfig.custom_inputs[i];

        std::ifstream file(
            ci.file, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error(
                "Failed to open custom input file: " + ci.file);
        }
        uint32_t fileSize = file.tellg();

        void* buffer_ptr = nullptr;

        if (needs_allocation) {
            // Allocate aligned memory for DSP (4096 bytes alignment)
            void* raw_ptr = nullptr;
            if (posix_memalign(&raw_ptr, 4096, fileSize) != 0) {
                // Fallback to regular malloc
                raw_ptr = malloc(fileSize);
            }
            std::shared_ptr<void> imageBuffer(raw_ptr, free);

            // Store the buffer so it stays alive during execution
            currentStaticBuffers.push_back(imageBuffer);

            std::ifstream embeddingStream(
                ci.file, std::ifstream::binary);
            embeddingStream.read(
                static_cast<char*>(imageBuffer.get()), fileSize);

            buffer_ptr = imageBuffer.get();
        } else {
            // Reuse existing memory buffer to avoid invalidating DSP mappings
            buffer_ptr = currentStaticBuffers[i].get();
        }

        std::cout << "[VLMObject::loadStaticCustomInputs] "
                  << (needs_allocation ? "Loaded " : "Reused ")
                  << fileSize << " bytes for " << ci.input_type
                  << " to buffer ptr: " << buffer_ptr << std::endl;

        // Determine target node
        std::shared_ptr<Node> targetNode;
        if (ci.node == "imageEncoder") {
            targetNode = imageEncoderNode;
        } else if (ci.node == "lutEncoder") {
            targetNode = lutEncoderNode;
        } else if (ci.node == "textGenerator") {
            targetNode = textGeneratorNode;
        } else {
            throw std::invalid_argument(
                "Unknown node in custom_inputs: " + ci.node);
        }

        // Convert input_type string to enum
        GenieNode_IOName_t ioEnum = stringToNodeIO(ci.input_type);

        // Set data on the node
        targetNode->setData(ioEnum, buffer_ptr, fileSize);
    }
    std::cout << "[VLMObject::loadStaticCustomInputs] "
              << "Finished loading custom inputs." << std::endl;
}

/*--------------------------------------------------------------------
 * Explicitly reset the VLM pipeline state
 *--------------------------------------------------------------------*/
void VLMObject::resetPipeline() {
    if (!pipeline) {
        throw std::runtime_error(
            "Pipeline not initialized - cannot reset");
    }

    try {
        pipeline->reset();
    } catch (const std::exception& e) {
        std::cout << "Error resetting pipeline: " << e.what()
                  << std::endl;
        throw;
    }
}

/*--------------------------------------------------------------------
 * VLM completion – main entry point for a request
 *--------------------------------------------------------------------*/
void VLMObject::vlm_chat_completion_create() {
    std::cout << "[VLMObject::vlm_chat_completion_create] "
              << "Entering..." << std::endl;
    if (!query) {
        throw std::runtime_error("VLMObject query not initialized.");
    }

    // Clear previous image data to ensure fresh state for each
    // request. This prevents stale image data from previous
    // requests
    currentImageData.clear();
    std::cout << "[VLMObject::vlm_chat_completion_create] "
              << "Cleared previous image data buffer" << std::endl;

    // Check if Sampling Parameters are used
    if (query->temperature != 1 || query->top_p != 1 ||
        query->top_k > 0 ||
        query->presence_penalty != 0.0 ||
        query->frequency_penalty != 0.0) {
        SamplerConfig sc;
        textGeneratorNode->getSampler();
        sc.createSamplerConfig(sc_configPath);
        if (query->temperature != 1) {
            sc.setParam("temp", std::to_string(query->temperature));
        }
        if (query->top_p != 1) {
            sc.setParam("top-p", std::to_string(query->top_p));
        }
        if (query->top_k > 0) {
            sc.setParam("top-k", std::to_string(query->top_k));
        }
        if (query->presence_penalty != 0.0) {
            sc.setParam("presence-penalty",
                std::to_string(query->presence_penalty));
        }
        if (query->frequency_penalty != 0.0) {
            sc.setParam("frequency-penalty",
                std::to_string(query->frequency_penalty));
        }
        textGeneratorNode->applyConfig(sc());
    }

    // ---------------------------------------------------------------
    // 1. Extract user text prompt and image buffer from the query
    // ---------------------------------------------------------------
    std::string userPrompt;

    if (query->message.use_content_items) {
        for (int i = 0; i < query->message.content_items_count; ++i) {
            const ContentItem& item = query->message.content_items[i];
            if (item.type == CONTENT_TYPE_TEXT) {
                userPrompt = item.text;
            } else if (item.type == CONTENT_TYPE_IMAGE_BUFFER) {
                // Copy the user's buffer into the member variable
                // currentImageData. This ensures the buffer stays
                // alive throughout pipeline execution.
                const void* userBuffer = item.image.buffer;
                size_t bufferSize = item.image.size;
                std::cout << "[VLMObject::vlm_chat_completion_create] "
                          << "Image buffer size: " << bufferSize
                          << ", ptr: " << userBuffer << std::endl;

                if (userBuffer != nullptr && bufferSize > 0) {
                    // Resize and copy image data into member
                    // variable
                    currentImageData.resize(bufferSize);
                    std::copy(
                        static_cast<const uint8_t*>(userBuffer),
                        static_cast<const uint8_t*>(userBuffer) +
                            bufferSize,
                        currentImageData.begin());

                    std::cout
                        << "[VLMObject::vlm_chat_completion_create] "
                        << "Copied " << bufferSize
                        << " bytes of image data to member buffer "
                        << "at ptr: "
                        << (void*)currentImageData.data()
                        << std::endl;

                    imageEncoderNode->setData(
                        GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,
                        currentImageData.data(),
                        currentImageData.size());
                    std::cout
                        << "[VLMObject::vlm_chat_completion_create] "
                        << "Set image data on imageEncoderNode."
                        << std::endl;
                }
            }
        }
    } else {
        // Backward‑compatible LLM‑only mode (should not happen
        // for VLM)
        userPrompt = query->message.content;
    }

    // ---------------------------------------------------------------
    // 2. Split prompt at vision token boundaries and interleave setData
    // ---------------------------------------------------------------
    const std::string& visionStart = modelConfig.visionStartToken;
    const std::string& visionEnd   = modelConfig.visionEndToken;
    size_t startPos = userPrompt.find(visionStart);
    size_t endPos   = userPrompt.find(visionEnd);

    if (startPos != std::string::npos && endPos != std::string::npos && endPos > startPos) {
        // --- Interleaved path: pre-vision text → image → post-vision text ---
        currentPreVisionText  = userPrompt.substr(0, startPos + visionStart.length());
        currentPostVisionText = userPrompt.substr(endPos);

        // (a) Pre-vision text → LUT encoder
        lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, currentPreVisionText);

        // (b) Image data → Image encoder
        if (!currentImageData.empty()) {
            imageEncoderNode->setData(
                GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,
                currentImageData.data(),
                currentImageData.size());
        }

        // Load static custom inputs (position ids, masks, etc.)
        loadStaticCustomInputs();

        // (c) Post-vision text → LUT encoder
        lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, currentPostVisionText);
    } else {
        // --- Non-interleaved path: set prompt as-is ---
        currentPromptData = userPrompt;
        lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, currentPromptData);

        // Reload static inputs (Pos IDs, Masks) for every request.
        loadStaticCustomInputs();
    }

    // ---------------------------------------------------------------
    // 4. Execute the pipeline
    // ---------------------------------------------------------------
    VLMUserData userData;
    userData.stream = &stream;
    userData.vlmObj = this;
    userData.cv = &cv;
    userData.request_in_progress = &request_in_progress;

    request_in_progress = true;
    bool execution_succeeded = false;
    std::exception_ptr execution_error = nullptr;

    try {
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Preparing to call pipeline->execute()..."
                  << std::endl;
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Calling pipeline->execute()..." << std::endl;
        pipeline->execute(&userData);
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "pipeline->execute() returned successfully!"
                  << std::endl;

        // Wait for the callback to signal completion
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Waiting for condition variable..."
                  << std::endl;
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]{ return !request_in_progress; });
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Condition variable wait finished."
                  << std::endl;

        execution_succeeded = true;

        // CRITICAL: Add delay here to ensure SDK's callback
        // threads have fully completed. The callback signals us,
        // but the SDK may still be cleaning up internally.
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Starting 1-second SDK cleanup delay..."
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "[VLMObject::vlm_chat_completion_create] "
                  << "Callback completed, SDK cleanup delay finished"
                  << std::endl;

    } catch (const std::exception& e) {
        std::cout << "ERROR: Pipeline execution failed: "
                  << e.what() << std::endl;
        request_in_progress = false;
        execution_error = std::current_exception();
    } catch (...) {
        std::cout << "ERROR: Pipeline execution failed with "
                  << "unknown exception" << std::endl;
        request_in_progress = false;
        execution_error = std::current_exception();
    }

    // Always attempt to reset pipeline state, even on error
    try {
        pipeline->reset();
        std::cout << "Pipeline reset completed successfully"
                  << std::endl;

        // Add delay to ensure hardware resources (DSP/NPU) are
        // fully released
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "Hardware stabilization delay completed, "
                  << "ready for next request" << std::endl;

        if (!execution_succeeded) {
            std::cout << "Pipeline reset completed after "
                      << "execution failure" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "ERROR: Failed to reset pipeline: "
                  << e.what() << std::endl;

        if (!execution_succeeded) {
            // Both execution and reset failed - critical state
            throw std::runtime_error(
                "Pipeline in inconsistent state: execution "
                "failed and reset failed. Pipeline may need to "
                "be recreated.");
        } else {
            // Execution succeeded but reset failed - warn but
            // don't fail the request
            std::cout << "WARNING: Request completed but pipeline "
                      << "reset failed. Next request may encounter "
                      << "issues." << std::endl;
        }
    }

    // Re-throw execution error if one occurred
    if (execution_error) {
        std::rethrow_exception(execution_error);
    }
}

/*--------------------------------------------------------------------
 * Static callback handling text output from the text generator
 *--------------------------------------------------------------------*/
Genie_Status_t VLMObject::textOutputCallback(
    const char* responseStr,
    GenieNode_TextOutput_SentenceCode_t sentenceCode,
    const void* userData) {
    // This callback is invoked from an external C library. It MUST
    // NOT throw exceptions.
    try {
        std::cout << "[textOutputCallback] Invoked. sentenceCode: "
                  << sentenceCode;
        if (responseStr) {
            std::cout << ", responseStr length: "
                      << std::strlen(responseStr);
        } else {
            std::cout << ", responseStr: null";
        }
        std::cout << std::endl;

        VLMUserData* udata = static_cast<VLMUserData*>(
            const_cast<void*>(userData));
        if (!udata || !udata->vlmObj || !udata->stream ||
            !udata->cv || !udata->request_in_progress) {
            std::cout << "[textOutputCallback] ERROR: Critical "
                      << "user data is null." << std::endl;
            return GENIE_STATUS_ERROR_INVALID_ARGUMENT;
        }

        bool isStreaming = *udata->stream;
        bool isEndOfSentence =
            (sentenceCode == GENIE_NODE_SENTENCE_END);
        // Enhanced end-of-stream detection: explicitly check for
        // END, ABORT, or COMPLETE.
        bool isEndOfStream =
            (sentenceCode == GENIE_NODE_SENTENCE_END ||
             sentenceCode == GENIE_NODE_SENTENCE_ABORT ||
             sentenceCode == GENIE_NODE_SENTENCE_COMPLETE);

        std::cout << "[textOutputCallback] isStreaming: "
                  << isStreaming << ", isEndOfSentence: "
                  << isEndOfSentence << ", isEndOfStream: "
                  << isEndOfStream << std::endl;

        // Per-token callback: use lightweight TokenResponse
        if (responseStr && !isEndOfSentence) {
            if (udata->vlmObj->tokenCallback) {
                std::unique_ptr<TokenResponse> token = std::make_unique<TokenResponse>();
                strlcpy(token->id, udata->vlmObj->id, sizeof(token->id));
                strlcpy(token->model, udata->vlmObj->modelSelected, sizeof(token->model));
                strlcpy(token->content, responseStr, sizeof(token->content));
                token->finish_reason[0] = '\0';  // Empty for intermediate tokens

                udata->vlmObj->tokenCallback(token.get());
            } else {
                std::cout << "[textOutputCallback] WARNING: tokenCallback is null, token lost!" << std::endl;
            }
        }

        // Send final token with finish_reason
        if (isEndOfSentence) {
            if (udata->vlmObj->tokenCallback) {
                std::unique_ptr<TokenResponse> token = std::make_unique<TokenResponse>();
                strlcpy(token->id, udata->vlmObj->id, sizeof(token->id));
                strlcpy(token->model, udata->vlmObj->modelSelected, sizeof(token->model));
                if (responseStr) {
                    strlcpy(token->content, responseStr, sizeof(token->content));
                } else {
                    token->content[0] = '\0';
                }
                strlcpy(token->finish_reason, "stop", sizeof(token->finish_reason));

                udata->vlmObj->tokenCallback(token.get());
            } else {
                std::cout << "[textOutputCallback] WARNING: tokenCallback is null, final token lost!" << std::endl;
            }
        }

        // If the stream has ended (for any reason), notify the
        // waiting thread.
        if (isEndOfStream) {
            std::cout << "[textOutputCallback] End of stream "
                      << "detected. Notifying condition variable..."
                      << std::endl;
            // IMPORTANT: Acquire the mutex before modifying the
            // condition variable's shared state to avoid data races
            // and undefined behavior.
            {
                std::lock_guard<std::mutex> lock(udata->vlmObj->mtx);
                *udata->request_in_progress = false;
            }
            udata->cv->notify_one();
            std::cout << "[textOutputCallback] Condition variable "
                      << "notified." << std::endl;
        }

        std::cout << "[textOutputCallback] Finished successfully."
                  << std::endl;
        return GENIE_STATUS_SUCCESS;

    } catch (const std::exception& e) {
        std::cout << "ERROR: Exception caught in "
                  << "textOutputCallback: " << e.what() << std::endl;
        // Attempt to unblock the waiting thread even on error.
        VLMUserData* udata = static_cast<VLMUserData*>(
            const_cast<void*>(userData));
        if (udata && udata->vlmObj && udata->request_in_progress &&
            udata->cv) {
            {
                std::lock_guard<std::mutex> lock(udata->vlmObj->mtx);
                *udata->request_in_progress = false;
            }
            udata->cv->notify_one();
        }
        // Signal an error back to the SDK.
        return GENIE_STATUS_ERROR_QUERY_FAILED;
    } catch (...) {
        std::cout << "ERROR: Unknown exception caught in "
                  << "textOutputCallback" << std::endl;
        // Attempt to unblock the waiting thread even on error.
        VLMUserData* udata = static_cast<VLMUserData*>(
            const_cast<void*>(userData));
        if (udata && udata->vlmObj && udata->request_in_progress &&
            udata->cv) {
            {
                std::lock_guard<std::mutex> lock(udata->vlmObj->mtx);
                *udata->request_in_progress = false;
            }
            udata->cv->notify_one();
        }
        // Signal an error back to the SDK.
        return GENIE_STATUS_ERROR_QUERY_FAILED;
    }
}
