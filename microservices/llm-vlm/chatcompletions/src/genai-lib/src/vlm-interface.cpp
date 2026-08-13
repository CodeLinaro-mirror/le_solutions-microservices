/*=============================================================================
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 *=============================================================================*/

#include "vlm-interface.h"
#include "vlm-service.hpp"
#include <iostream>
#include <string>
#include <cstdio>

std::string vlm_get_error_message(const std::string& error_str) {
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

VLMHandle vlm_create_object(const char* model, const char* config_path,
                            const char* sampler_config_path, bool streaming) {
    try {
        // Create a new VLMObject with safe string construction
        const std::string model_str = model ? model : "";
        const std::string config_str = config_path ? config_path : "";
        const std::string sampler_str = sampler_config_path ?
                                        sampler_config_path : "sampler.json";

        // Create a new VLMObject and return it as an opaque handle
        return new VLMObject(model_str, config_str, sampler_str, streaming);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in vlm_create_object: "
                  << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in vlm_create_object"
                  << std::endl;
        return nullptr;
    }
}

void vlm_destroy_object(VLMHandle handle) {
    try {
        // Delete the VLMObject instance
        delete static_cast<VLMObject*>(handle);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in vlm_destroy_object: "
                  << e.what() << std::endl;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in vlm_destroy_object"
                  << std::endl;
    }
}

void vlm_chat_completion_create(VLMHandle handle,
                                 const Query* query,
                                 bool streaming,
                                 LLMTokenCallback token_cb) {
    if (!handle) return;
    try {
        VLMObject* obj = static_cast<VLMObject*>(handle);

        obj->stream = streaming;
        obj->tokenCallback = token_cb;

        *(obj->query) = *query;

        obj->vlm_chat_completion_create();
    } catch (const std::exception& e) {
        std::string err_msg = e.what();
        std::cerr << "ERROR: Exception caught in vlm_chat_completion_create_with_token_callback: "
                  << err_msg << std::endl;
        // Send error via token callback with finish_reason="error"
        if (token_cb) {
            std::string layman_msg = vlm_get_error_message(err_msg);
            TokenResponse errorToken;
            snprintf(errorToken.id, sizeof(errorToken.id), "error");
            snprintf(errorToken.model, sizeof(errorToken.model), "vlm");
            snprintf(errorToken.content, sizeof(errorToken.content),
                     "%s", layman_msg.c_str());
            snprintf(errorToken.finish_reason, sizeof(errorToken.finish_reason),
                     "error");
            token_cb(&errorToken);
        }
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in "
                  << "vlm_chat_completion_create_with_token_callback" << std::endl;
        if (token_cb) {
            std::string layman_msg = vlm_get_error_message("");
            TokenResponse errorToken;
            snprintf(errorToken.id, sizeof(errorToken.id), "error");
            snprintf(errorToken.model, sizeof(errorToken.model), "vlm");
            snprintf(errorToken.content, sizeof(errorToken.content),
                     "%s", layman_msg.c_str());
            snprintf(errorToken.finish_reason, sizeof(errorToken.finish_reason),
                     "error");
            token_cb(&errorToken);
        }
    }
}

} // extern "C"
