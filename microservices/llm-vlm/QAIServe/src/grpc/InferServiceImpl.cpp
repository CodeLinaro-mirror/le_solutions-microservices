// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "grpc/InferServiceImpl.h"
#include "grpc/GrpcErrorMapping.h"
#include "qai_forge/QaiForge.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/dto/TensorDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace {

std::string generateRequestId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "infer-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

TensorInferenceRequest BuildRequest(const qai::forge::v1::InferRequest* proto_req) {
    TensorInferenceRequest dto;
    dto.model = proto_req->model();
    dto.request_id = proto_req->request_id().empty() ? generateRequestId()
                                                       : proto_req->request_id();

    for (const auto& t : proto_req->inputs()) {
        InputTensor tensor;
        tensor.name = t.name();
        tensor.shape.assign(t.shape().begin(), t.shape().end());
        tensor.dtype = tensorDataTypeFromString(t.datatype());
        tensor.data.assign(t.contents().begin(), t.contents().end());
        dto.inputs.push_back(std::move(tensor));
    }

    dto.output_names.assign(proto_req->output_names().begin(),
                             proto_req->output_names().end());
    return dto;
}

} // namespace

grpc::Status InferServiceImpl::Infer(
    grpc::ServerContext* /*context*/,
    const qai::forge::v1::InferRequest* request,
    qai::forge::v1::InferResponse* response) {
    const std::string& model_name = request->model();

    auto& cfg = ModelConfigManager::getInstance();
    if (!cfg.validateModel(model_name)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                             "Model '" + model_name + "' not found.");
    }
    if (cfg.getModelType(model_name) != "predictive") {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                             "Model '" + model_name + "' is a generative model. "
                             "Use ChatService.CreateChatCompletion instead.");
    }
    if (request->inputs().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                             "Missing required field: 'inputs'");
    }

    TensorInferenceRequest dto = BuildRequest(request);

    try {
        TensorInferenceResponse result = qai_forge::QaiForge::getInstance().infer(dto);

        response->set_model(!result.model.empty() ? result.model : dto.model);
        response->set_request_id(result.request_id);
        response->set_latency_ms(result.stats.latency_ms);
        response->set_backend_name(result.stats.backend_name);

        for (const auto& out : result.outputs) {
            auto* t = response->add_outputs();
            t->set_name(out.name);
            for (auto d : out.shape) t->add_shape(d);
            t->set_datatype(tensorDataTypeToString(out.dtype));
            t->set_contents(reinterpret_cast<const char*>(out.data.data()), out.data.size());
        }

        return grpc::Status::OK;
    } catch (const GenAIException& e) {
        return grpc::Status(grpc_util::MapErrorCode(e.code), e.message);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                             std::string("Internal error: ") + e.what());
    }
}
