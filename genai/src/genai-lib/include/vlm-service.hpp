//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#ifndef VLM_SERVICE_HPP
#define VLM_SERVICE_HPP

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstddef>

#include "GeniePipeline.h"
#include "GenieNode.h"
#include "GenieProfile.h"
#include "GenieSampler.h"
#include "llm-buffer.h"

class Profile
{
    public:
        std::string profilePath = "profile.txt";
        Profile();
        ~Profile();
        void getJsonData();
        GenieProfile_Handle_t getProfileHandle() { return m_handle; }
        GenieProfile_Handle_t operator()() const { return m_handle; }

    private:
        GenieProfile_Handle_t m_handle = NULL;
};

class SamplerConfig
{
    public:
        void createSamplerConfig(const std::string& configPath);
        std::string getConfigString() { return m_config; }
        void setParam(const std::string& keyStr, const std::string& valueStr);
        ~SamplerConfig();
        GenieSamplerConfig_Handle_t operator()() const { return m_handle; }
    private:
        GenieSamplerConfig_Handle_t m_handle = NULL;
        std::string m_config;
};

class Pipeline {
public:
    class Config {
    public:
        Config(const std::string& jsonConfig, std::shared_ptr<Profile> profile);
        ~Config();
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;
        Config(Config&& other) noexcept;
        Config& operator=(Config&& other);
        GeniePipelineConfig_Handle_t operator()() const { return m_handle; }
    private:
        GeniePipelineConfig_Handle_t m_handle = nullptr;
    };

    template <typename T>
    explicit Pipeline(T&& config);
    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&& other) noexcept;
    Pipeline& operator=(Pipeline&& other);

    inline void addNode(std::shared_ptr<class Node> node);
    inline void connect(std::shared_ptr<class Node> producerNode,
                        GenieNode_IOName_t producerIO,
                        std::shared_ptr<class Node> consumerNode,
                        GenieNode_IOName_t consumerIO);
    inline void execute(void* userData);
    inline void reset();
    template <typename T>
    inline void save(T&& path);
    template <typename T>
    inline void restore(T&& path);

    GeniePipeline_Handle_t operator()() const { return m_handle; }

private:
    GeniePipeline_Handle_t m_handle = nullptr;
};

class Node {
public:
    class Config {
    public:
        Config(const std::string& jsonConfig, std::shared_ptr<Profile> profile);
        ~Config();
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;
        Config(Config&& other) noexcept;
        Config& operator=(Config&& other);
        GenieNodeConfig_Handle_t operator()() const { return m_handle; }

    private:
        GenieNodeConfig_Handle_t m_handle = nullptr;
    };

    template <typename T>
    explicit Node(T&& config);
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&& other) noexcept;
    Node& operator=(Node&& other);

    void setData(GenieNode_IOName_t ioName, std::string text, const char* dataConfig = nullptr);
    void setData(GenieNode_IOName_t ioName, const void* data, const size_t dataSize, const char* dataConfig = nullptr);

    void setTextCallback(GenieNode_IOName_t ioName,
                         GenieNode_TextOutput_Callback_t callback);
    void setEmbeddingCallback(GenieNode_IOName_t ioName,
                              GenieNode_EmbeddingOutputCallback_t callback);
    void getSampler();
    void applyConfig(GenieSamplerConfig_Handle_t samplerConfigHandle);

    GenieNode_Handle_t operator()() const { return m_handle; }

private:
    GenieNode_Handle_t m_handle = nullptr;
    GenieSampler_Handle_t m_samplerHandle = NULL;
};

/*--------------------------------------------------------------
 * Template implementations for Pipeline
 *--------------------------------------------------------------*/
template <typename T>
Pipeline::Pipeline(T&& config) {
    const Genie_Status_t status = GeniePipeline_create(std::forward<T>(config)(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error("Failed to create the pipeline");
    }
}

template <typename T>
inline void Pipeline::save(T&& path) {
    const Genie_Status_t status = GeniePipeline_save(m_handle, std::forward<T>(path).c_str());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to save");
    }
}

template <typename T>
inline void Pipeline::restore(T&& path) {
    const Genie_Status_t status = GeniePipeline_restore(m_handle, std::forward<T>(path).c_str());
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to restore");
    }
}

/*--------------------------------------------------------------
 * Template implementations for Node
 *--------------------------------------------------------------*/
template <typename T>
Node::Node(T&& config) {
    const Genie_Status_t status = GenieNode_create(std::forward<T>(config)(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
        throw std::runtime_error("Failed to create the Genie Node");
    }
}

/*--------------------------------------------------------------
 * VLMObject – high‑level API for Vision‑Language Model inference
 *--------------------------------------------------------------*/
class VLMObject {
public:
    enum class VLMModel {
        QWEN2_5_VL_3B,
        UNKNOWN = -1
    };

    VLMObject(const std::string& model = "", const std::string& config_path = "", bool streaming = false);
    ~VLMObject();

    /* OpenAI‑compatible query */
    std::unique_ptr<Query> query;
    std::shared_ptr<Profile> profiler;

    char id[256];
    bool stream;
    char modelSelected[256];

    LLMResponseCallback responseCallback = nullptr;

    void vlm_chat_completion_create();   // Execute a VLM request

private:
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Node> imageEncoderNode;
    std::shared_ptr<Node> lutEncoderNode;
    std::shared_ptr<Node> textGeneratorNode;
    std::string sc_configPath;

    /* Keep binary image data alive for the duration of execution */
    std::vector<uint8_t> currentImageData;

    /* Helper structures */
    struct ModelConfig {
        std::string imageEncoderConfig;
        std::string lutEncoderConfig;
        std::string textGeneratorConfig;
        struct CustomInput {
            std::string node;
            std::string input_type;
            std::string file;
        };
        std::vector<CustomInput> custom_inputs;
    };

    ModelConfig modelConfig;

    /* Internal helpers */
    void loadConfig(const std::string& modelName);
    void createPipelineAndNodes();
    void connectNodes();
    void loadStaticCustomInputs();
    void setSystemPrompt();

    /* Callback for text output from the text generator node */
    static Genie_Status_t textOutputCallback(const char* responseStr,
                                             GenieNode_TextOutput_SentenceCode_t sentenceCode,
                                             const void* userData);
};


typedef struct {
    std::string* responseStr;
    bool* stream;
    VLMObject* vlmObj;
} VLMQueryMutex;

#endif // VLM_SERVICE_HPP
