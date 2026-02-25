//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "vlm-service.hpp"
#include <iostream> // Explicitly included for std::cerr
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <json/json.h>
#include <cstddef> // Explicitly included for size_t
#include <thread>   // For std::this_thread::sleep_for
#include <chrono>   // For std::chrono::seconds

Profile::Profile() {
    const int32_t status = GenieProfile_create(nullptr, &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
      throw std::runtime_error("Failed to create the profile handle.");
    }
}

void Profile::getJsonData() {
    const char* jsonData = nullptr;
    const Genie_AllocCallback_t callback([](size_t size, const char** data) {
      *data = (char*)malloc(size);
      if (*data == nullptr) {
        throw std::runtime_error("Cannot allocate memory for JSON data");
      }
  });

    const int32_t status = GenieProfile_getJsonData(m_handle, callback, &jsonData);
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to get the profile data");
    }

    std::ofstream outFile;
    outFile.open(profilePath);
    if (!outFile.good()) {
      throw std::runtime_error("Cannot create profile output file with name:" + profilePath);
    }
    outFile << jsonData;
    outFile.close();
    free((char*)jsonData);
}

Profile::~Profile() {
    const int32_t status = GenieProfile_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
      std::cerr << "Failed to free the profile handle." << std::endl;
    }
}

void SamplerConfig::createSamplerConfig(const std::string& configPath) {
    std::ifstream confStream(configPath);
    std::string config;
    std::getline(confStream, config, '\0');
    m_config = config;
    const int32_t status = GenieSamplerConfig_createFromJson(config.c_str(), &m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to create sampler config.");
    }
}

void SamplerConfig::setParam(const std::string& keyStr, const std::string& valueStr) {
    const int32_t status = GenieSamplerConfig_setParam(m_handle, keyStr.c_str(), valueStr.c_str());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to setParam");
    }
}

SamplerConfig::~SamplerConfig() {
    const int32_t status = GenieSamplerConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cerr << "Failed to free the sampler config." << std::endl;
    }
}

/*--------------------------------------------------------------
 * Helper: convert string to GenieNode_IOName_t
 *--------------------------------------------------------------*/
static GenieNode_IOName_t stringToNodeIO(const std::string& nodeIOString) {
    static const std::unordered_map<std::string, GenieNode_IOName_t> nodeIOMap = {
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
    throw std::invalid_argument("Invalid Node IO value passed: " + nodeIOString);
}

/*--------------------------------------------------------------
 * Pipeline::Config implementation
 *--------------------------------------------------------------*/
Pipeline::Config::Config(const std::string& jsonConfig,
                        std::shared_ptr<Profile> profile) : m_handle(nullptr) {
    const Genie_Status_t status = GeniePipelineConfig_createFromJson(
        jsonConfig.c_str(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error("Failed to create the pipeline config");
    }
    if (profile) {
        const Genie_Status_t bindStatus = GeniePipelineConfig_bindProfiler(m_handle, (*profile)());
        if (GENIE_STATUS_SUCCESS != bindStatus) {
            throw std::runtime_error("Failed to bind the profile handle with the pipeline config");
        }
    }
}

Pipeline::Config::~Config() {
    const Genie_Status_t status = GeniePipelineConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cerr << "Failed to free the pipeline config." << std::endl;
    }
}

Pipeline::Config::Config(Config&& other) noexcept : m_handle(nullptr) {
    *this = std::move(other);
}

Pipeline::Config& Pipeline::Config::operator=(Config&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

/*--------------------------------------------------------------
 * Pipeline implementation
 *--------------------------------------------------------------*/
Pipeline::~Pipeline() {
    const Genie_Status_t status = GeniePipeline_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cerr << "Failed to free the pipeline." << std::endl;
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
    const Genie_Status_t status = GeniePipeline_addNode(m_handle, (*node)());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to add node");
    }
}

inline void Pipeline::connect(std::shared_ptr<Node> producerNode,
                              GenieNode_IOName_t producerIO,
                              std::shared_ptr<Node> consumerNode,
                              GenieNode_IOName_t consumerIO) {
    const Genie_Status_t status = GeniePipeline_connect(
        m_handle, (*producerNode)(), producerIO, (*consumerNode)(), consumerIO);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to connect");
    }
}

inline void Pipeline::execute(void* userData) {
    // Blocking call: expected to return only after the pipeline finishes and
    // all callbacks using 'userData' have completed. If this changes to async,
    // the VLMObject must be updated to manage 'userData' lifetime accordingly.
    const Genie_Status_t status = GeniePipeline_execute(m_handle, userData);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to execute");
    }
}

inline void Pipeline::reset() {
    const Genie_Status_t status = GeniePipeline_reset(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to reset");
    }
}

/*--------------------------------------------------------------
 * Node::Config implementation
 *--------------------------------------------------------------*/
Node::Config::Config(const std::string& jsonConfig,
                     std::shared_ptr<Profile> profile) : m_handle(nullptr) {
    const Genie_Status_t status = GenieNodeConfig_createFromJson(
        jsonConfig.c_str(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error("Failed to create the node config");
    }
    if (profile) {
        const Genie_Status_t bindStatus = GenieNodeConfig_bindProfiler(m_handle, (*profile)());
        if (GENIE_STATUS_SUCCESS != bindStatus) {
            throw std::runtime_error("Failed to bind the profile handle with the node config");
        }
    }
}

Node::Config::~Config() {
    const Genie_Status_t status = GenieNodeConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cerr << "Failed to free the node config." << std::endl;
    }
}

Node::Config::Config(Config&& other) noexcept : m_handle(nullptr) {
    *this = std::move(other);
}

Node::Config& Node::Config::operator=(Config&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

/*--------------------------------------------------------------
 * Node implementation
 *--------------------------------------------------------------*/
Node::~Node() {
    const Genie_Status_t status = GenieNode_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
        std::cerr << "Failed to free the Genie Node." << std::endl;
    }
}

Node::Node(Node&& other) noexcept : m_handle(nullptr) {
    *this = std::move(other);
}

Node& Node::operator=(Node&& other) {
    std::swap(m_handle, other.m_handle);
    return *this;
}

void Node::setData(GenieNode_IOName_t ioName, std::string text, const char* dataConfig) {
    const Genie_Status_t status =
        GenieNode_setData(m_handle, ioName, (void*)(text.c_str()), text.size(), dataConfig);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to set the text input data");
    }
}

void Node::setData(GenieNode_IOName_t ioName,
                   const void* data,
                   const size_t dataSize,
                   const char* dataConfig) {
    const Genie_Status_t status =
        GenieNode_setData(m_handle, ioName, (void*)data, dataSize, dataConfig);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to set the embedding input data");
    }
}

void Node::setTextCallback(GenieNode_IOName_t ioName,
                           GenieNode_TextOutput_Callback_t callback) {
    const Genie_Status_t status = GenieNode_setTextCallback(m_handle, ioName, callback);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to set the text output callback");
    }
}

void Node::setEmbeddingCallback(GenieNode_IOName_t ioName,
                                GenieNode_EmbeddingOutputCallback_t callback) {
    const Genie_Status_t status = GenieNode_setEmbeddingCallback(m_handle, ioName, callback);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to set the embedding output callback");
    }
}

void Node::getSampler() {
    const int32_t status = GenieNode_getSampler(m_handle, &m_samplerHandle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to get sampler.");
    }
}

void Node::applyConfig(GenieSamplerConfig_Handle_t samplerConfigHandle) {
    const int32_t status = GenieSampler_applyConfig(m_samplerHandle, samplerConfigHandle);
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to apply sampler config.");
    }
}

/*--------------------------------------------------------------
 * VLMObject implementation
 *--------------------------------------------------------------*/
VLMObject::VLMObject(const std::string& model, const std::string& config_path, bool streaming) {
    stream = streaming;
    query = std::make_unique<Query>();
    strlcpy(modelSelected, model.c_str(), sizeof(modelSelected));

    // Load configuration from JSON file if provided, otherwise use hard-coded defaults
    if (!config_path.empty()) {
        loadConfig(config_path);
    } else {
        std::string modelName = model.empty() ? "Qwen2.5-VL-3B" : model;
        loadConfig(modelName);
    }

    // Create profiler (shared with all configs)
    profiler = std::make_shared<Profile>();

    // Build pipeline and nodes
    createPipelineAndNodes();

    // Connect nodes
    connectNodes();

    // Load static custom inputs (position ids, masks, etc.)
    loadStaticCustomInputs();

    sc_configPath = "sampler.json";
}

VLMObject::~VLMObject() {
    // All smart pointers clean up automatically
}

/*--------------------------------------------------------------
 * Load model configuration from JSON file or use hard-coded defaults
 *--------------------------------------------------------------*/
void VLMObject::loadConfig(const std::string& configPathOrModelName) {
    // Check if this is a file path (contains .json) or a model name
    if (configPathOrModelName.find(".json") != std::string::npos) {
        // Load from JSON file
        std::ifstream configFile(configPathOrModelName);
        if (!configFile.is_open()) {
            throw std::runtime_error("Failed to open config file: " + configPathOrModelName);
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errs;

        if (!Json::parseFromStream(builder, configFile, &root, &errs)) {
            throw std::runtime_error("Failed to parse JSON config: " + errs);
        }

        // Get the first (and only) model configuration
        if (root.empty()) {
            throw std::runtime_error("Empty JSON configuration file");
        }

        // Get the first model key
        std::string modelKey = root.getMemberNames()[0];
        const Json::Value& modelConfig_json = root[modelKey];

        // Validate required fields
        if (!modelConfig_json.isMember("pipeline") ||
            !modelConfig_json["pipeline"].isMember("nodes")) {
            throw std::runtime_error("Invalid JSON structure: missing 'pipeline.nodes'");
        }

        const Json::Value& nodes = modelConfig_json["pipeline"]["nodes"];

        // Extract node configuration paths
        if (!nodes.isMember("imageEncoder") || !nodes.isMember("lutEncoder") ||
            !nodes.isMember("textGenerator")) {
            throw std::runtime_error("Invalid JSON structure: missing required node configurations");
        }

        modelConfig.imageEncoderConfig = nodes["imageEncoder"].asString();
        modelConfig.lutEncoderConfig = nodes["lutEncoder"].asString();
        modelConfig.textGeneratorConfig = nodes["textGenerator"].asString();

        // Parse custom_inputs array
        modelConfig.custom_inputs.clear();
        if (modelConfig_json.isMember("custom_inputs") && modelConfig_json["custom_inputs"].isArray()) {
            const Json::Value& customInputs = modelConfig_json["custom_inputs"];
            for (const auto& input : customInputs) {
                if (!input.isMember("node") || !input.isMember("input_type") ||
                    !input.isMember("file")) {
                    throw std::runtime_error("Invalid custom_input entry: missing required fields");
                }

                ModelConfig::CustomInput ci;
                ci.node = input["node"].asString();
                ci.input_type = input["input_type"].asString();
                ci.file = input["file"].asString();
                modelConfig.custom_inputs.push_back(ci);
            }
        }
    } else {
        // Use hard-coded configuration for backward compatibility
        if (configPathOrModelName != "QWEN2_5_VL_3B" && configPathOrModelName != "Qwen2.5-VL-3B") {
            throw std::invalid_argument("Unsupported VLM model: " + configPathOrModelName);
        }

        // Populate modelConfig with hard-coded values
        modelConfig.imageEncoderConfig = "qwen_veg.json";
        modelConfig.lutEncoderConfig   = "text-encoder.json";
        modelConfig.textGeneratorConfig = "qwen-htp.json";

        // Custom static inputs for the image encoder
        modelConfig.custom_inputs = {
            {"imageEncoder", "GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS", "position_ids_cos.raw"},
            {"imageEncoder", "GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN", "position_ids_sin.raw"},
            {"imageEncoder", "GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK", "window_attention_mask.raw"},
            {"imageEncoder", "GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK", "full_attention_mask.raw"}
        };
    }
}

/*--------------------------------------------------------------
 * Create pipeline and node objects
 *--------------------------------------------------------------*/
void VLMObject::createPipelineAndNodes() {
    // Pipeline config (empty JSON string – we only need a handle to bind profiler)
    auto pipelineCfg = std::make_shared<Pipeline::Config>("", profiler);
    pipeline = std::make_shared<Pipeline>(std::move(*pipelineCfg));

    // Create image encoder node
    std::string imgCfgStr;
    {
        std::ifstream f(modelConfig.imageEncoderConfig);
        if (!f) throw std::runtime_error("Failed to open " + modelConfig.imageEncoderConfig);
        std::getline(f, imgCfgStr, '\0');
    }
    auto imgNodeCfg = std::make_shared<Node::Config>(imgCfgStr, profiler);
    imageEncoderNode = std::make_shared<Node>(std::move(*imgNodeCfg));
    pipeline->addNode(imageEncoderNode);
    std::cerr << "Created image encoder node" << std::endl;

    // LUT encoder node
    std::string lutCfgStr;
    {
        std::ifstream f(modelConfig.lutEncoderConfig);
        if (!f) throw std::runtime_error("Failed to open " + modelConfig.lutEncoderConfig);
        std::getline(f, lutCfgStr, '\0');
    }
    auto lutNodeCfg = std::make_shared<Node::Config>(lutCfgStr, profiler);
    lutEncoderNode = std::make_shared<Node>(std::move(*lutNodeCfg));
    pipeline->addNode(lutEncoderNode);

    // Create text generator node
    std::string txtGenCfgStr;
    {
        std::ifstream f(modelConfig.textGeneratorConfig);
        if (!f) throw std::runtime_error("Failed to open " + modelConfig.textGeneratorConfig);
        std::getline(f, txtGenCfgStr, '\0');
    }
    auto txtGenNodeCfg = std::make_shared<Node::Config>(txtGenCfgStr, profiler);
    textGeneratorNode = std::make_shared<Node>(std::move(*txtGenNodeCfg));
    pipeline->addNode(textGeneratorNode);
    std::cerr << "Created text generator node" << std::endl;
}

/*--------------------------------------------------------------
 * Connect the three nodes inside the pipeline
 *--------------------------------------------------------------*/
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

/*--------------------------------------------------------------
 * Load static custom inputs (position ids, masks, etc.)
 *--------------------------------------------------------------*/
void VLMObject::loadStaticCustomInputs() {
    for (const auto& ci : modelConfig.custom_inputs) {
        // Read binary file
        std::ifstream file(ci.file, std::ios::binary | std::ios::ate);
        if (!file) {
            throw std::runtime_error("Failed to open custom input file: " + ci.file);
        }
        uint32_t fileSize = file.tellg();
        std::shared_ptr<void> imageBuffer(new int8_t[fileSize], [](void* p) { delete[] static_cast<int8_t*>(p); });
        std::ifstream embeddingStream(ci.file, std::ifstream::binary);
        embeddingStream.read(static_cast<char*>(imageBuffer.get()), fileSize);

        // Determine target node
        std::shared_ptr<Node> targetNode;
        if (ci.node == "imageEncoder") {
            targetNode = imageEncoderNode;
        } else if (ci.node == "lutEncoder") {
            targetNode = lutEncoderNode;
        } else if (ci.node == "textGenerator") {
            targetNode = textGeneratorNode;
        } else {
            throw std::invalid_argument("Unknown node in custom_inputs: " + ci.node);
        }

        // Convert input_type string to enum
        GenieNode_IOName_t ioEnum = stringToNodeIO(ci.input_type);

        // Set data on the node
        targetNode->setData(ioEnum, imageBuffer.get(), fileSize);
    }
}

/*--------------------------------------------------------------
 * Explicitly reset the VLM pipeline state
 *--------------------------------------------------------------*/
void VLMObject::resetPipeline() {
    if (!pipeline) {
        throw std::runtime_error("Pipeline not initialized - cannot reset");
    }

    try {
        pipeline->reset();
    } catch (const std::exception& e) {
        std::cerr << "Error resetting pipeline: " << e.what() << std::endl;
        throw;
    }
}

/*--------------------------------------------------------------
 * VLM completion – main entry point for a request
 *--------------------------------------------------------------*/
void VLMObject::vlm_chat_completion_create() {
    if (!query) {
        throw std::runtime_error("VLMObject query not initialized.");
    }

    // Clear previous image data to ensure fresh state for each request
    // This prevents stale image data from previous requests
    currentImageData.clear();
    std::cerr << "Cleared previous image data buffer" << std::endl;

    //Check if Sampling Parameters are used
    if (query->temperature != 1 || query->top_p != 1 || query->presence_penalty != 0.0 || query->frequency_penalty != 0.0){
        SamplerConfig sc;
        textGeneratorNode->getSampler();
        sc.createSamplerConfig(sc_configPath);
        if (query->temperature != 1) {
            sc.setParam("temp", std::to_string(query->temperature));
        }
        if (query->top_p != 1) {
            sc.setParam("top-p", std::to_string(query->top_p));
        }
        if (query->presence_penalty != 0.0) {
            sc.setParam("presence-penalty", std::to_string(query->presence_penalty));
        }
        if (query->frequency_penalty != 0.0) {
            sc.setParam("frequency-penalty", std::to_string(query->frequency_penalty));
        }
        //diag->getSampler();
        textGeneratorNode->applyConfig(sc());
    }

    // -----------------------------------------------------------------
    // 1. Extract user text prompt and image buffer from the query
    // -----------------------------------------------------------------
    std::string userPrompt;

    if (query->message.use_content_items) {
        for (int i = 0; i < query->message.content_items_count; ++i) {
            const ContentItem& item = query->message.content_items[i];
            if (item.type == CONTENT_TYPE_TEXT) {
                userPrompt = item.text;
            } else if (item.type == CONTENT_TYPE_IMAGE_BUFFER) {
                // Copy the user's buffer into the member variable currentImageData
                // This ensures the buffer stays alive throughout pipeline execution
                const void* userBuffer = item.image.buffer;
                size_t bufferSize = item.image.size;

                if (userBuffer != nullptr && bufferSize > 0) {
                    // Resize and copy image data into member variable
                    currentImageData.resize(bufferSize);
                    std::copy(static_cast<const uint8_t*>(userBuffer),
                              static_cast<const uint8_t*>(userBuffer) + bufferSize,
                              currentImageData.begin());

                    std::cerr << "Copied " << bufferSize << " bytes of image data to member buffer" << std::endl;

                    imageEncoderNode->setData(
                        GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,
                        currentImageData.data(),
                        currentImageData.size());
                }
            }
        }
    } else {
        // Backward‑compatible LLM‑only mode (should not happen for VLM)
        userPrompt = query->message.content;
    }

    // -----------------------------------------------------------------
    // 3. Build the final prompt that will be fed to the LUT encoder
    // -----------------------------------------------------------------
    // The prompt is now fully constructed in the Python layer, including
    // chat template markers and system prompt. We pass it as-is.
    std::string completePrompt = userPrompt;

    lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, completePrompt);

    // Reload static inputs (Pos IDs, Masks) for every request
    // This is required because ImageEncoder clears its input buffer after each encoding
    try {
        loadStaticCustomInputs();
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to reload static custom inputs: " << e.what() << std::endl;
        throw;
    }

    // -----------------------------------------------------------------
    // 4. Execute the pipeline
    // -----------------------------------------------------------------
    std::string responseText;
    VLMUserData userData;
    userData.responseStr = &responseText;
    userData.stream = &stream;
    userData.vlmObj = this;
    userData.cv = &cv;
    userData.request_in_progress = &request_in_progress;

    request_in_progress = true;
    bool execution_succeeded = false;
    std::exception_ptr execution_error = nullptr;

    try {
        pipeline->execute(&userData);

        // Wait for the callback to signal completion
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this]{ return !request_in_progress; });

        execution_succeeded = true;

        // CRITICAL: Add delay here to ensure SDK's callback threads have fully completed
        // The callback signals us, but the SDK may still be cleaning up internally
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cerr << "Callback completed, SDK cleanup delay finished" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: Pipeline execution failed: " << e.what() << std::endl;
        request_in_progress = false;
        execution_error = std::current_exception();
    } catch (...) {
        std::cerr << "ERROR: Pipeline execution failed with unknown exception" << std::endl;
        request_in_progress = false;
        execution_error = std::current_exception();
    }

    // Always attempt to reset pipeline state, even on error
    try {
        pipeline->reset();
        std::cerr << "Pipeline reset completed successfully" << std::endl;

        // Add delay to ensure hardware resources (DSP/NPU) are fully released
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cerr << "Hardware stabilization delay completed, ready for next request" << std::endl;

        if (!execution_succeeded) {
            std::cerr << "Pipeline reset completed after execution failure" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed to reset pipeline: " << e.what() << std::endl;

        if (!execution_succeeded) {
            // Both execution and reset failed - critical state
            throw std::runtime_error(
                "Pipeline in inconsistent state: execution failed and reset failed. "
                "Pipeline may need to be recreated.");
        } else {
            // Execution succeeded but reset failed - warn but don't fail the request
            std::cerr << "WARNING: Request completed but pipeline reset failed. "
                      << "Next request may encounter issues." << std::endl;
        }
    }

    // Re-throw execution error if one occurred
    if (execution_error) {
        std::rethrow_exception(execution_error);
    }
}

/*--------------------------------------------------------------
 * Static callback handling text output from the text generator
 *--------------------------------------------------------------*/
Genie_Status_t VLMObject::textOutputCallback(const char* responseStr,
                                             GenieNode_TextOutput_SentenceCode_t sentenceCode,
                                             const void* userData) {
    // This callback is invoked from an external C library. It MUST NOT throw exceptions.
    try {
        VLMUserData* udata = static_cast<VLMUserData*>(const_cast<void*>(userData));
        if (!udata || !udata->vlmObj || !udata->stream || !udata->cv || !udata->request_in_progress) {
            std::cerr << "[textOutputCallback] ERROR: Critical user data is null." << std::endl;
            return GENIE_STATUS_ERROR_INVALID_ARGUMENT;
        }

        bool isStreaming = *udata->stream;
        bool isEndOfSentence = (sentenceCode == GENIE_NODE_SENTENCE_END);
        // Enhanced end-of-stream detection: explicitly check for END, ABORT, or COMPLETE
        bool isEndOfStream = (sentenceCode == GENIE_NODE_SENTENCE_END ||
                              sentenceCode == GENIE_NODE_SENTENCE_ABORT ||
                              sentenceCode == GENIE_NODE_SENTENCE_COMPLETE);

        if (isStreaming) {
            // Streaming mode: send each token immediately.
            std::unique_ptr<Response> resp = std::make_unique<Response>();
            strlcpy(resp->model, udata->vlmObj->modelSelected, sizeof(resp->model));
            Message msg;
            strlcpy(msg.role, "assistant", sizeof(msg.role));
            if (responseStr) {
                strlcpy(msg.content, responseStr, sizeof(msg.content));
            }
            resp->choices[0].message = msg;

            if (isEndOfSentence) {
                strlcpy(resp->choices[0].finish_reason, "stop", sizeof(resp->choices[0].finish_reason));
            }

            if (udata->vlmObj->responseCallback) {
                udata->vlmObj->responseCallback(resp.get());
            }
        } else {
            // Non-streaming mode: accumulate the full response.
            if (responseStr && udata->responseStr) {
                *(udata->responseStr) += responseStr;
            }

            if (isEndOfSentence) {
                // End of sentence: send the complete response.
                std::unique_ptr<Response> resp = std::make_unique<Response>();
                strlcpy(resp->model, udata->vlmObj->modelSelected, sizeof(resp->model));
                Message msg;
                strlcpy(msg.role, "assistant", sizeof(msg.role));
                strlcpy(msg.content, udata->responseStr->c_str(), sizeof(msg.content));
                resp->choices[0].message = msg;
                strlcpy(resp->choices[0].finish_reason, "stop", sizeof(resp->choices[0].finish_reason));

                if (udata->vlmObj->responseCallback) {
                    udata->vlmObj->responseCallback(resp.get());
                }
            }
        }

        // If the stream has ended (for any reason), notify the waiting thread.
        if (isEndOfStream) {
            *udata->request_in_progress = false;
            udata->cv->notify_one();
        }

        return GENIE_STATUS_SUCCESS;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in textOutputCallback: " << e.what() << std::endl;
        // Attempt to unblock the waiting thread even on error.
        VLMUserData* udata = static_cast<VLMUserData*>(const_cast<void*>(userData));
        if (udata && udata->request_in_progress && udata->cv) {
            *udata->request_in_progress = false;
            udata->cv->notify_one();
        }
        return GENIE_STATUS_ERROR_QUERY_FAILED; // Signal an error back to the SDK.
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in textOutputCallback" << std::endl;
        // Attempt to unblock the waiting thread even on error.
        VLMUserData* udata = static_cast<VLMUserData*>(const_cast<void*>(userData));
        if (udata && udata->request_in_progress && udata->cv) {
            *udata->request_in_progress = false;
            udata->cv->notify_one();
        }
        return GENIE_STATUS_ERROR_QUERY_FAILED; // Signal an error back to the SDK.
    }
}
