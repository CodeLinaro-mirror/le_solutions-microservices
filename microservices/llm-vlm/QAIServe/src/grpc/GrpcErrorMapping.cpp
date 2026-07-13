// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "grpc/GrpcErrorMapping.h"

namespace grpc_util {

grpc::StatusCode MapErrorCode(GenAIErrorCode code) {
    switch (code) {
        case GenAIErrorCode::MODEL_NOT_FOUND:
            return grpc::StatusCode::NOT_FOUND;
        case GenAIErrorCode::CONTEXT_LENGTH_EXCEEDED:
            return grpc::StatusCode::INVALID_ARGUMENT;
        case GenAIErrorCode::TOOL_RESPONSE_TIMEOUT:
            return grpc::StatusCode::DEADLINE_EXCEEDED;
        case GenAIErrorCode::INFERENCE_FAILED:
            return grpc::StatusCode::INTERNAL;
        case GenAIErrorCode::INSUFFICIENT_MEMORY:
            return grpc::StatusCode::RESOURCE_EXHAUSTED;
        case GenAIErrorCode::HARDWARE_UNAVAILABLE:
            return grpc::StatusCode::UNAVAILABLE;
        case GenAIErrorCode::INVALID_REQUEST:
            return grpc::StatusCode::INVALID_ARGUMENT;
        case GenAIErrorCode::INTERNAL_ERROR:
        default:
            return grpc::StatusCode::INTERNAL;
    }
}

} // namespace grpc_util
