// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge.grpc.pb.h"

class InferServiceImpl final : public qai::forge::v1::InferService::Service {
public:
    grpc::Status Infer(
        grpc::ServerContext* context,
        const qai::forge::v1::InferRequest* request,
        qai::forge::v1::InferResponse* response) override;
};
