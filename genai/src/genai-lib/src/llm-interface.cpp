//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================
#include "llm-interface.h"
#include "llm-service.hpp"

extern "C" {

LLMHandle llm_create_object(const char* model, const char* config_path, bool streaming) { //Creates LLM Object
    try {
        return new LLMObject(std::string(model), std::string(config_path), streaming);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to create LLM object: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "[ERROR] Failed to create LLM object: Unknown error" << std::endl;
        return nullptr;
    }
}

void llm_destroy_object(LLMHandle handle) { //Destroys Handle
    delete static_cast<LLMObject*>(handle);
}

void llm_reset_object(LLMHandle handle) { //Resets Dialog
   if (handle) {
        try {
            LLMObject* obj = static_cast<LLMObject*>(handle);
            obj->resetDialog();
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to reset LLM object: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[ERROR] Failed to reset LLM object: Unknown error" << std::endl;
        }
   }
}

void llm_chat_completion_create(LLMHandle handle, const Query* query, bool streaming,
                                LLMResponseCallback cb) { //Calls Chat Completion Create
   if (handle) {
        try {
            LLMObject* obj = static_cast<LLMObject*>(handle);
            // Set the Response callback
            obj->responseCallback = cb;
            // Update streaming mode
            obj->stream = streaming;
            //First populate Query struct in LLM Object with the input struct from REST API side
            *(obj->query) = *query;
            // Call chat completion create to run query
            obj->chat_completion_create();
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to create chat completion: " << e.what() << std::endl;
            // Try to send error response if callback is available
            if (cb) {
                Response errorResponse;
                Message errorMessage;
                strlcpy(errorMessage.role, "assistant", sizeof(errorMessage.role));
                strlcpy(errorMessage.content, "Error processing request", sizeof(errorMessage.content));
                errorResponse.choices[0].message = errorMessage;
                strlcpy(errorResponse.choices[0].finish_reason, "error", sizeof(errorResponse.choices[0].finish_reason));
                cb(&errorResponse);
            }
        } catch (...) {
            std::cerr << "[ERROR] Failed to create chat completion: Unknown error" << std::endl;
        }
    }
}

}
