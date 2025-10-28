//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "vlm-service.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <json/json.h>

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

    // Set the static system prompt on the LUT encoder
    setSystemPrompt();

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

    // Image encoder node
    std::string imgCfgStr;
    {
        std::ifstream f(modelConfig.imageEncoderConfig);
        if (!f) throw std::runtime_error("Failed to open " + modelConfig.imageEncoderConfig);
        std::getline(f, imgCfgStr, '\0');
    }
    auto imgNodeCfg = std::make_shared<Node::Config>(imgCfgStr, profiler);
    imageEncoderNode = std::make_shared<Node>(std::move(*imgNodeCfg));
    pipeline->addNode(imageEncoderNode);

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

    // Text generator node
    std::string txtGenCfgStr;
    {
        std::ifstream f(modelConfig.textGeneratorConfig);
        if (!f) throw std::runtime_error("Failed to open " + modelConfig.textGeneratorConfig);
        std::getline(f, txtGenCfgStr, '\0');
    }
    auto txtGenNodeCfg = std::make_shared<Node::Config>(txtGenCfgStr, profiler);
    textGeneratorNode = std::make_shared<Node>(std::move(*txtGenNodeCfg));
    pipeline->addNode(textGeneratorNode);
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
 * Set the static system prompt on the LUT encoder node
 *--------------------------------------------------------------*/
void VLMObject::setSystemPrompt() {
    std::string systemPrompt =
        "<|im_start|>system\\nYou are a helpful assistant.<|im_end|>\\n"
        "<|im_start|>user\\n<|vision_start|>";
    lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, systemPrompt);
}

/*--------------------------------------------------------------
 * VLM completion – main entry point for a request
 *--------------------------------------------------------------*/
void VLMObject::vlm_chat_completion_create() {
    if (!query) {
        throw std::runtime_error("VLMObject query not initialized.");
    }

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
    std::shared_ptr<void> managedImageBuffer;

    if (query->message.use_content_items) {
        for (int i = 0; i < query->message.content_items_count; ++i) {
            const ContentItem& item = query->message.content_items[i];
            if (item.type == CONTENT_TYPE_TEXT) {
                userPrompt = item.text;
            } else if (item.type == CONTENT_TYPE_IMAGE_BUFFER) {
                // Copy the user's buffer into a managed buffer to ensure it stays alive
                const void* userBuffer = item.image.buffer;
                size_t bufferSize = item.image.size;

                if (userBuffer != nullptr && bufferSize > 0) {
                    managedImageBuffer = std::shared_ptr<void>(new int8_t[bufferSize], [](void* p) { delete[] static_cast<int8_t*>(p); });
                    std::copy(static_cast<const int8_t*>(userBuffer),
                              static_cast<const int8_t*>(userBuffer) + bufferSize,
                              static_cast<int8_t*>(managedImageBuffer.get()));

                    imageEncoderNode->setData(
                        GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,
                        managedImageBuffer.get(),
                        bufferSize);
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
    std::string finalPrompt =
        "<|vision_end|> " + userPrompt + " <|im_end|>\\n<|im_start|>assistant";

    lutEncoderNode->setData(GENIE_NODE_TEXT_ENCODER_TEXT_INPUT, finalPrompt);

    // -----------------------------------------------------------------
    // 4. Execute the pipeline
    // -----------------------------------------------------------------
    std::string responseText;
    VLMQueryMutex qmtx;
    qmtx.responseStr = &responseText;
    qmtx.stream = &stream;
    qmtx.vlmObj = this;

    pipeline->execute(&qmtx);

    qmtx.responseStr = nullptr;
    qmtx.stream = nullptr;
    qmtx.vlmObj = nullptr;
}

/*--------------------------------------------------------------
 * Static callback handling text output from the text generator
 *--------------------------------------------------------------*/
Genie_Status_t VLMObject::textOutputCallback(const char* responseStr,
                                             GenieNode_TextOutput_SentenceCode_t sentenceCode,
                                             const void* userData) {
    VLMQueryMutex* qmtx = static_cast<VLMQueryMutex*>(const_cast<void*>(userData));
    if (!qmtx) return GENIE_STATUS_ERROR_INVALID_ARGUMENT;

    // Non‑streaming mode: accumulate full response until END
    if (!(*qmtx->stream)) {
        if (responseStr && qmtx->responseStr) {
            *(qmtx->responseStr) += responseStr;
        }

        if (sentenceCode == GENIE_NODE_SENTENCE_END) {
            // Build OpenAI‑compatible Response object
            std::unique_ptr<Response> resp = std::make_unique<Response>();
            strlcpy(resp->model, qmtx->vlmObj->modelSelected, sizeof(resp->model));

            Message msg;
            strlcpy(msg.role, "assistant", sizeof(msg.role));
            strlcpy(msg.content, qmtx->responseStr->c_str(), sizeof(msg.content));
            resp->choices[0].message = msg;

            if (qmtx->vlmObj->responseCallback) {
                qmtx->vlmObj->responseCallback(resp.get());
            }
        }
    } else {
        // Streaming mode – send each token / chunk immediately
        std::unique_ptr<Response> resp = std::make_unique<Response>();
        strlcpy(resp->model, qmtx->vlmObj->modelSelected, sizeof(resp->model));

        Message msg;
        strlcpy(msg.role, "assistant", sizeof(msg.role));
        if (responseStr) {
            strlcpy(msg.content, responseStr, sizeof(msg.content));
        }
        resp->choices[0].message = msg;

        if (qmtx->vlmObj->responseCallback) {
            qmtx->vlmObj->responseCallback(resp.get());
        }

        if (sentenceCode == GENIE_NODE_SENTENCE_END) {
            // Send a final empty message with finish_reason = "stop"
            std::unique_ptr<Response> endResp = std::make_unique<Response>();
            Message endMsg;
            strlcpy(endMsg.content, "", sizeof(endMsg.content));
            endResp->choices[0].message = endMsg;
            strlcpy(endResp->choices[0].finish_reason, "stop", sizeof(endResp->choices[0].finish_reason));
            qmtx->vlmObj->responseCallback(endResp.get());
        }
    }

    return GENIE_STATUS_SUCCESS;
}
