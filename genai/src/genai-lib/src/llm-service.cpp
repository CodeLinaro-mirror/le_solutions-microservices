//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "llm-service.hpp"

namespace {
struct ReasoningMarkerPair {
    std::string begin;
    std::string end;
};

const std::vector<ReasoningMarkerPair> kReasoningMarkers = {
    {"<think>", "</think>"},
};

size_t longestSuffixPrefix(const std::string& data, const std::string& token) {
    const size_t max_len = std::min(token.size() - 1, data.size());
    for (size_t len = max_len; len > 0; --len) {
        if (data.compare(data.size() - len, len, token, 0, len) == 0) {
            return len;
        }
    }
    return 0;
}

size_t longestSuffixPrefixAny(const std::string& data, const std::vector<ReasoningMarkerPair>& markers) {
    size_t best = 0;
    for (const auto& marker : markers) {
        const size_t suffix = longestSuffixPrefix(data, marker.begin);
        if (suffix > best) {
            best = suffix;
        }
    }
    return best;
}

bool findNextBegin(const std::string& data,
                   size_t start,
                   const std::vector<ReasoningMarkerPair>& markers,
                   size_t& out_pos,
                   const ReasoningMarkerPair*& out_marker) {
    size_t best_pos = std::string::npos;
    const ReasoningMarkerPair* best_marker = nullptr;
    for (const auto& marker : markers) {
        size_t pos = data.find(marker.begin, start);
        if (pos != std::string::npos && (best_marker == nullptr || pos < best_pos)) {
            best_pos = pos;
            best_marker = &marker;
        }
    }
    if (best_marker == nullptr) {
        return false;
    }
    out_pos = best_pos;
    out_marker = best_marker;
    return true;
}

std::string filterThinkBlocks(const std::string& chunk, QueryStruct* qmtx) {
    std::string data = qmtx->think_carry + chunk;
    qmtx->think_carry.clear();

    std::string out;
    size_t pos = 0;

    while (pos < data.size()) {
        if (qmtx->in_think) {
            size_t end_pos = data.find(qmtx->active_think_end, pos);
            if (end_pos == std::string::npos) {
                size_t suffix = longestSuffixPrefix(data, qmtx->active_think_end);
                if (suffix > 0) {
                    qmtx->think_carry = data.substr(data.size() - suffix);
                }
                return out;
            }
            pos = end_pos + qmtx->active_think_end.size();
            qmtx->in_think = false;
            qmtx->active_think_end.clear();
            continue;
        }

        size_t begin_pos = std::string::npos;
        const ReasoningMarkerPair* marker = nullptr;
        if (!findNextBegin(data, pos, kReasoningMarkers, begin_pos, marker)) {
            size_t suffix = longestSuffixPrefixAny(data, kReasoningMarkers);
            if (suffix > 0) {
                out.append(data, pos, data.size() - pos - suffix);
                qmtx->think_carry = data.substr(data.size() - suffix);
            } else {
                out.append(data, pos, std::string::npos);
            }
            return out;
        }

        out.append(data, pos, begin_pos - pos);
        pos = begin_pos + marker->begin.size();
        qmtx->in_think = true;
        qmtx->active_think_end = marker->end;
    }

    return out;
}
}  // namespace

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

Dialog::Config::Config(const std::string& config, std::shared_ptr<Profile> profile) {
    int32_t status = GenieDialogConfig_createFromJson(config.c_str(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
      throw std::runtime_error("Failed to create the dialog config.");
    }

    if (profile) {
      m_profileHandle = profile->getProfileHandle();
      status          = GenieDialogConfig_bindProfiler(m_handle, m_profileHandle);
      if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to bind the profile handle with the dialog config.");
      }
    }
}

Dialog::Config::~Config() {
    int32_t status = GenieDialogConfig_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
      std::cerr << "Failed to free the dialog config." << std::endl;
    }
}

Dialog::Dialog(Config config) {
    int32_t status = GenieDialog_create(config(), &m_handle);
    if ((GENIE_STATUS_SUCCESS != status) || (!m_handle)) {
      throw std::runtime_error("Failed to create the dialog.");
    }
}

Dialog::~Dialog() {
    int32_t status = GenieDialog_free(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
      std::cerr << "Failed to free the dialog." << std::endl;
    }
}

void Dialog::query(const std::string prompt, GenieDialog_SentenceCode_t sentencCode,
  void* userData) {
    const int32_t status =
        GenieDialog_query(m_handle, prompt.c_str(), sentencCode, queryCallback, userData);
    if (GENIE_STATUS_WARNING_ABORTED == status) {
      std::cout << "Query Succesfully aborted" << std::endl;
    } else if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to query.");
    }
}

void Dialog::save(const std::string name) {
    int32_t status = GenieDialog_save(m_handle, name.c_str());
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to save.");
    }
}

void Dialog::restore(const std::string name) {
    int32_t status = GenieDialog_restore(m_handle, name.c_str());
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to restore.");
    }
}
void Dialog::getSampler() {
    const int32_t status = GenieDialog_getSampler(m_handle, &m_samplerHandle);
    if (GENIE_STATUS_SUCCESS != status) {
        throw std::runtime_error("Failed to get sampler.");
    }
}

void Dialog::applySamplerConfig(GenieSamplerConfig_Handle_t samplerConfigHandle) {
    const int32_t status = GenieSampler_applyConfig(m_samplerHandle, samplerConfigHandle);
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to apply sampler config.");
    }
}

void Dialog::setMaxNumTokens(const int maxNumTokens) {
    const int32_t status = GenieDialog_setMaxNumTokens(m_handle, maxNumTokens);
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to set max num tokens.");
    }
}

void Dialog::reset() {
    int32_t status = GenieDialog_reset(m_handle);
    if (GENIE_STATUS_SUCCESS != status) {
      throw std::runtime_error("Failed to reset the dialog KV cache.");
    }
}

void Dialog::queryCallback(const char* responseStr,
    const GenieDialog_SentenceCode_t sentenceCode,
    const void* userData) {

    QueryStruct* qmtx = static_cast<QueryStruct*>(const_cast<void*>(userData));
    if (*qmtx->stream == false) { // Non Streaming
        if (responseStr && qmtx->responseStr) {
          std::string filtered = filterThinkBlocks(responseStr, qmtx);
          if (!filtered.empty()) {
            *(qmtx->responseStr) += filtered;
          }
        }

        if (sentenceCode == GENIE_DIALOG_SENTENCE_END) {
          qmtx->in_think = false;
          qmtx->think_carry.clear();
          qmtx->active_think_end.clear();

          std::unique_ptr<Response> response = std::make_unique<Response>();
          strlcpy(response->model, qmtx->llmObj->modelSelected, sizeof(response->model));
          Message message;

          strlcpy(message.role, "assistant", sizeof(message.role));
          strlcpy(message.content, qmtx->responseStr->c_str(), sizeof(message.content));

          response->choices[0].message = message;
          strlcpy(response->choices[0].finish_reason, "stop", sizeof(response->choices[0].finish_reason));

          if (qmtx->llmObj && qmtx->llmObj->responseCallback) {
            qmtx->llmObj->responseCallback(response.get());
          } else {
            std::cout << "Callback NOT Registered" << std::endl;
          }
        }
    } else { // Streaming
      std::unique_ptr<Response> response = std::make_unique<Response>();
      strlcpy(response->model, qmtx->llmObj->modelSelected, sizeof(response->model));
      Message message;
      strlcpy(message.role, "assistant", sizeof(message.role));

      if (responseStr) {
        std::string filtered = filterThinkBlocks(responseStr, qmtx);
        if (!filtered.empty()) {
          //Token by Token
          strlcpy(message.content, filtered.c_str(), sizeof(message.content));
          response->choices[0].message = message;

          if (qmtx->llmObj && qmtx->llmObj->responseCallback) {
            qmtx->llmObj->responseCallback(response.get());
          } else {
            std::cout << "Callback NOT Registered" << std::endl;
          }
        }
      }

      if (sentenceCode == GENIE_DIALOG_SENTENCE_END) {
        qmtx->in_think = false;
        qmtx->think_carry.clear();
        qmtx->active_think_end.clear();

        std::unique_ptr<Response> endResponse = std::make_unique<Response>();
        Message message;

        strlcpy(message.content, "", sizeof(message.content));
        endResponse->choices[0].message = message;
        strlcpy(endResponse->choices[0].finish_reason, "stop",
            sizeof(endResponse->choices[0].finish_reason));
        qmtx->llmObj->responseCallback(endResponse.get());
      }
    }
}

LLMObject::LLMObject(std::string model, std::string config_path, std::string sampler_config_path, bool streaming) {
    query = std::make_unique<Query>();
    profiler = std::make_shared<Profile>();
    stream = streaming;

    strlcpy(modelSelected, model.c_str(), sizeof(modelSelected));

    // Load config from provided path
    std::cout << "Loading model config from: " << config_path << std::endl;
    std::ifstream configStream(config_path);

    if (!configStream.is_open()) {
        throw std::runtime_error("Failed to open config file: " + std::string(config_path));
    }

    std::getline(configStream, config, '\0');
    configStream.close();

    std::cout << "Successfully loaded config for model: " << model << std::endl;

    sc_configPath = sampler_config_path.empty() ? "sampler.json" : sampler_config_path;
    diag = new Dialog(Dialog::Config(config, profiler));
}

void LLMObject::resetDialog() {
    diag->reset();
}

void LLMObject::chat_completion_create () {
    prompt = query->message.content;

    //Check if Sampling Parameters are used
    if (query->temperature != 1 || query->top_p != 1 || query->top_k > 0 ||
        query->presence_penalty != 0.0 || query->frequency_penalty != 0.0){
        SamplerConfig sc;
        diag->getSampler();
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
            sc.setParam("presence-penalty", std::to_string(query->presence_penalty));
        }
        if (query->frequency_penalty != 0.0) {
            sc.setParam("frequency-penalty", std::to_string(query->frequency_penalty));
        }
        diag->applySamplerConfig(sc());
    }

    //Check if max completion tokens is set
    if (query->max_completion_tokens != 0) {
        diag->setMaxNumTokens(query->max_completion_tokens);
    }

    // Add Query to History
    conversation.push_back(query->message);

    std::string responseText;
    QueryStruct qmtx;

    qmtx.responseStr = &responseText;
    qmtx.stream = &stream;
    qmtx.llmObj = this;
    qmtx.in_think = false;
    qmtx.think_carry.clear();
    qmtx.active_think_end.clear();

    diag->query(prompt, GenieDialog_SentenceCode_t::GENIE_DIALOG_SENTENCE_COMPLETE, &qmtx);

    qmtx.responseStr = nullptr;
    qmtx.stream = nullptr;
    qmtx.llmObj = nullptr;
}
