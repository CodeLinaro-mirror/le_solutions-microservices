/*=============================================================================
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 *=============================================================================*/

#include "vlm-interface.h"
#include "vlm-service.hpp"
#include <iostream>

extern "C" {

VLMHandle vlm_create_object(const char* model, const char* config_path, bool streaming) {
    try {
        // Create a new VLMObject with safe string construction
        const std::string model_str = model ? model : "";
        const std::string config_str = config_path ? config_path : "";

        // Create a new VLMObject and return it as an opaque handle
        return new VLMObject(model_str, config_str, streaming);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in vlm_create_object: " << e.what() << std::endl;
        return nullptr;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in vlm_create_object" << std::endl;
        return nullptr;
    }
}

void vlm_destroy_object(VLMHandle handle) {
    try {
        // Delete the VLMObject instance
        delete static_cast<VLMObject*>(handle);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in vlm_destroy_object: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in vlm_destroy_object" << std::endl;
    }
}

void vlm_chat_completion_create(VLMHandle handle,
                                const Query* query,
                                bool streaming,
                                LLMResponseCallback cb) {
    if (!handle) return;
    try {
        VLMObject* obj = static_cast<VLMObject*>(handle);

        // IMPORTANT: The same VLM handle may be reused for multiple requests (singleton/handle cache).
        // The streaming mode is a per-request behavior that influences the text callback logic.
        // Make sure to update the object's streaming flag for each call.
        obj->stream = streaming;

        // Set the response callback for this request. The callback reads the 'stream' flag above.
        obj->responseCallback = cb;

        // Copy the request payload. VLMObject owns this memory.
        *(obj->query) = *query;

        // Execute synchronously. All per-call lifetimes are scoped within this function.
        obj->vlm_chat_completion_create();
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Exception caught in vlm_chat_completion_create: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "ERROR: Unknown exception caught in vlm_chat_completion_create" << std::endl;
    }
}

} // extern "C"
