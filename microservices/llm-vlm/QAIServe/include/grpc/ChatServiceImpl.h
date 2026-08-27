// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge.grpc.pb.h"

class ChatServiceImpl final : public qai::forge::v1::ChatService::Service {
public:
    grpc::Status CreateChatCompletion(
        grpc::ServerContext* context,
        const qai::forge::v1::ChatCompletionRequest* request,
        qai::forge::v1::ChatCompletionResponse* response) override;

    grpc::Status CreateChatCompletionStream(
        grpc::ServerContext* context,
        const qai::forge::v1::ChatCompletionRequest* request,
        grpc::ServerWriter<qai::forge::v1::ChatCompletionChunk>* writer) override;
};
