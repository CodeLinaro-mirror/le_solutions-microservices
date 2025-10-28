/*=============================================================================
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 *=============================================================================*/

#include "vlm-interface.h"
#include "vlm-service.hpp"

extern "C" {

VLMHandle vlm_create_object(const char* model, const char* config_path, bool streaming) {
    // Create a new VLMObject and return it as an opaque handle
    return new VLMObject(std::string(model), std::string(config_path), streaming);
}

void vlm_destroy_object(VLMHandle handle) {
    // Delete the VLMObject instance
    delete static_cast<VLMObject*>(handle);
}

void vlm_chat_completion_create(VLMHandle handle,
                                const Query* query,
                                bool streaming,
                                LLMResponseCallback cb) {
    if (!handle) return;
    VLMObject* obj = static_cast<VLMObject*>(handle);
    // Set the response callback
    obj->responseCallback = cb;
    // Copy the query into the VLMObject (the VLMObject holds a unique_ptr<Query>)
    *(obj->query) = *query;
    // Perform the VLM completion
    obj->vlm_chat_completion_create();
}

}
