//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================
#include "llm-interface.h"
#include "llm-service.hpp"
#include <string>
#include <iostream>
#include <cstdio>

std::string llm_get_error_message(const std::string& error_str) {
    if (error_str.find("1002") != std::string::npos) {
        return "The system is not configured correctly to run AI models.";
    }
    if (error_str.find("1003") != std::string::npos) {
        return "This AI model cannot be run on your current hardware.";
    }
    if (error_str.find("5000") != std::string::npos) {
        return "This AI model is not compatible with the system's "
               "current software version.";
    }
    if (error_str.find("6000") != std::string::npos) {
        return "This AI model is not supported on this system.";
    }
    if (error_str.find("14003") != std::string::npos) {
        return "The system encountered an issue while cleaning up memory.";
    }
    return "System resources are busy. Consider using a model with "
           "a smaller context size in your requests.";
}

extern "C" {

LLMHandle llm_create_object(const char* model, const char* config_path,
                            const char* sampler_config_path, bool streaming) {
    try {
        const char* sampler_str = sampler_config_path ?
                                  sampler_config_path : "sampler.json";
        return new LLMObject(std::string(model), std::string(config_path),
                             std::string(sampler_str), streaming);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create LLM object: "
                  << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "[ERROR] Failed to create LLM object: Unknown error"
                  << std::endl;
        return nullptr;
    }
}

void llm_destroy_object(LLMHandle handle) {
    delete static_cast<LLMObject*>(handle);
}

void llm_reset_object(LLMHandle handle) {
    if (handle) {
        try {
            LLMObject* obj = static_cast<LLMObject*>(handle);
            obj->resetDialog();
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to reset LLM object: "
                      << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[ERROR] Failed to reset LLM object: Unknown error"
                      << std::endl;
        }
    }
}

void llm_chat_completion_create(LLMHandle handle, const Query* query,
                                bool streaming, LLMTokenCallback token_cb) {
    if (handle) {
        try {
            LLMObject* obj = static_cast<LLMObject*>(handle);
            obj->tokenCallback = token_cb;
            obj->stream = streaming;
            *(obj->query) = *query;
            obj->chat_completion_create();
        } catch (const std::exception& e) {
            std::string err_msg = e.what();
            std::cerr << "[ERROR] Failed to create chat completion: "
                      << err_msg << std::endl;
            if (token_cb) {
                std::string layman_msg = llm_get_error_message(err_msg);
                TokenResponse errorToken;
                snprintf(errorToken.id, sizeof(errorToken.id), "error");
                snprintf(errorToken.model, sizeof(errorToken.model), "llm");
                snprintf(errorToken.content, sizeof(errorToken.content),
                         "%s", layman_msg.c_str());
                snprintf(errorToken.finish_reason, sizeof(errorToken.finish_reason),
                         "error");
                token_cb(&errorToken);
            }
        } catch (...) {
            std::cerr << "[ERROR] Failed to create chat completion: "
                      << "Unknown error" << std::endl;
            if (token_cb) {
                std::string layman_msg = llm_get_error_message("");
                TokenResponse errorToken;
                snprintf(errorToken.id, sizeof(errorToken.id), "error");
                snprintf(errorToken.model, sizeof(errorToken.model), "llm");
                snprintf(errorToken.content, sizeof(errorToken.content),
                         "%s", layman_msg.c_str());
                snprintf(errorToken.finish_reason, sizeof(errorToken.finish_reason),
                         "error");
                token_cb(&errorToken);
            }
        }
    }
}

}
