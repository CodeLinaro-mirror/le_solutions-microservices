//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "llm-service.hpp"

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

void Dialog::queryCallback(const char* responseStr,
    const GenieDialog_SentenceCode_t sentenceCode,
    const void* userData) {

      QueryMutex* qmtx = static_cast<QueryMutex*>(const_cast<void*>(userData));

      if (responseStr && qmtx->responseStr){
        *(qmtx->responseStr) += responseStr;
      }

      if (sentenceCode == GENIE_DIALOG_SENTENCE_END){
        std::unique_lock<std::mutex> lock(*qmtx->mtx);
        *(qmtx->queryDone) = true;
        qmtx->cv->notify_one();
      }
}

LLMObject::LLMObject(std::string model){
    std::string file;
    query = std::make_unique<Query>();
    response = std::make_unique<Response>();
    profiler = std::make_shared<Profile>();
    strlcpy(response->model, model.c_str(), sizeof(response->model));
    response->model[sizeof(response->model) - 1] = '\0';

  //Utilize model to select correct genie_config for certain model
      LLMModel selectedModel = getModelFromQuery(model);

    switch(selectedModel) {
        case LLMModel::LLAMA3_1_8B: {
            file = "genie_config_llama3_1_8B.json";
            std::ifstream configStream(file);

            if (!configStream.is_open()) {
                std::cerr << "Failed to open config file: " << file << std::endl;
            }

            std::getline(configStream, config, '\0');
            break;
        }
        case LLMModel::LLAMA3_2_3B: {
            file = "genie_config_llama3_2_3B.json";
            std::ifstream configStream(file);

            if (!configStream.is_open()) {
                std::cerr << "Failed to open config file: " << file << std::endl;
            }

            std::getline(configStream, config, '\0');
            break;
        }
        case LLMModel::QWEN2_5_7B: {
            file = "genie_config_qwen2_5_7B.json";
            std::ifstream configStream(file);

            if (!configStream.is_open()) {
                std::cerr << "Failed to open config file: " << file << std::endl;
            }

            std::getline(configStream, config, '\0');
            break;
        }
        default: {
            std::cout << "ERROR Unsupported model selected" << std::endl;
            break;
        }
    }
    diag = new Dialog(Dialog::Config(config, profiler));
}

LLMObject::LLMModel LLMObject::getModelFromQuery(const std::string& model){
    static const std::unordered_map<std::string, LLMModel> modelMap = {
        {"LLAMA3_1_8B", LLMModel::LLAMA3_1_8B},
        {"LLAMA3_2_3B", LLMModel::LLAMA3_2_3B},
        {"QWEN2_5_7B", LLMModel::QWEN2_5_7B}
    };
    auto check = modelMap.find(model);
    if (check != modelMap.end()){
        return check->second;
    }
    else{
        std::cout << "Error Model not FOUND" << std::endl;
        return LLMModel::UNKNOWN;
    }
}

void LLMObject::constructPrompt(const std::string query, LLMModel model){
    Message message;
    switch(model) {
        case LLMModel::LLAMA3_1_8B:
            std::cout << "Llama 3.1 8B Model Selected. Assembling Prompt Format" << std::endl;

            prompt = "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n" + query +
                "<|eot_id|><|start_header_id|>assistant<|end_header_id|>";
            strlcpy(message.role, "user", sizeof(message.role));

            strlcpy(message.content, query.c_str(), sizeof(message.content));

            conversation.push_back(message);
            break;
        case LLMModel::LLAMA3_2_3B:
            std::cout << "Llama 3.2 3B Model Selected. Assembling Prompt Format" << std::endl;

            prompt = "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n" + query +
                "<|eot_id|><|start_header_id|>assistant<|end_header_id|>";
            strlcpy(message.role, "user", sizeof(message.role));

            strlcpy(message.content, query.c_str(), sizeof(message.content));
            conversation.push_back(message);
            break;
        case LLMModel::QWEN2_5_7B:
            std::cout << "Qwen 2.5 7B Model Selected. Assembling Prompt Format" << std::endl;

            prompt = "<|im_start|>system\nYou are a helpful AI Assistant<|im_end|><|im_start|>user\n"
                + query + "\n<|im_end|>\n<|im_start|>assistant\n";
            strlcpy(message.role, "user", sizeof(message.role));

            strlcpy(message.content, query.c_str(), sizeof(message.content));
            conversation.push_back(message);
            break;
        default:
            std::cout << "ERROR Unsupported model selected" << std::endl;
            break;
    }
}

void LLMObject::chat_completion_create () {
    prompt = query->message.content;
    LLMModel model = getModelFromQuery(query->model);

    // Add Query to History
    conversation.push_back(query->message);

    constructPrompt(prompt, model);

    std::mutex mtx;
    std::condition_variable cv;
    bool queryDone = false;
    std::string responseText;

    QueryMutex qmtx;
    qmtx.mtx = &mtx;
    qmtx.cv = &cv;
    qmtx.queryDone = &queryDone;
    qmtx.responseStr = &responseText;

    diag->query(prompt, GenieDialog_SentenceCode_t::GENIE_DIALOG_SENTENCE_COMPLETE, &qmtx);
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return queryDone; });
    }

    Message message;
    strlcpy(message.role, "assistant", sizeof(message.role));

    strlcpy(message.content, responseText.c_str(), sizeof(message.content));
    response->choices[0].message = message;

    // Add Response to History
    conversation.push_back(message);
}

void LLMObject::chat_completion_retrieve () {
  // Web API will call this function to get a certain LLM Object
  // This function will return that LLM Object requested
}

void LLMObject::chat_completion_list () {
  // THis function when called by Web API will List ALL of the LLMObjects
}

void LLMObject::chat_completion_delete () {
  // Web API will call this function to delete a specified LLM Object
}

void LLMObject::chat_completion_messages_list() {
  //This function will list ALL the messages from the specified LLM Object
}

/* Example of how to call LLM Object and print a specific message
  chat_completion.content[0].message.content
  LLMObject.message.content
  messages=[{"role": "user", "content": "What's a good name for a bakery?"}]

*/
