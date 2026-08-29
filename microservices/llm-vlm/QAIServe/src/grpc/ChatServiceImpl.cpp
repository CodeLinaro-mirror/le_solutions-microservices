// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "grpc/ChatServiceImpl.h"
#include "grpc/GrpcErrorMapping.h"
#include "qai_forge/QaiForge.h"
#include "qai_forge/InternalDTOs.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>

// CreateChatCompletionRequest, StandardResponse, StreamChunk, GenAIException,
// and GenAIErrorCode are declared in the global namespace by InternalDTOs.h
// (only QaiForge, GenerateOptions, and StreamCallbacks live in qai_forge::).

namespace {

CreateChatCompletionRequest BuildRequest(
    const qai::forge::v1::ChatCompletionRequest* proto_req) {
    CreateChatCompletionRequest dto;
    dto.model = proto_req->model();
    dto.messages = json::array();
    for (const auto& m : proto_req->messages()) {
        dto.messages.push_back({{"role", m.role()}, {"content", m.content()}});
    }
    if (proto_req->max_tokens() > 0) {
        dto.max_completion_tokens = proto_req->max_tokens();
    }
    if (proto_req->temperature() > 0) {
        dto.temperature = proto_req->temperature();
    }
    if (proto_req->top_p() > 0) {
        dto.top_p = proto_req->top_p();
    }
    if (proto_req->top_k() > 0) {
        dto.top_k = proto_req->top_k();
    }
    if (!proto_req->user().empty()) {
        dto.user = proto_req->user();
    }
    return dto;
}

std::string GenerateTurnId() {
    static std::atomic<uint64_t> next_id{1};
    return "grpc_chat_turn_" + std::to_string(next_id.fetch_add(1));
}

qai_forge::GenerateOptions BuildOptions(
    const qai::forge::v1::ChatCompletionRequest* request) {
    qai_forge::GenerateOptions options;
    const std::string turn_id = GenerateTurnId();
    options.response_id = turn_id;
    options.session_id = request->user().empty() ? turn_id : request->user();
    if (!request->user().empty() && request->messages_size() > 0) {
        qai_forge::ConversationReference reference;
        reference.namespace_id = "qaiserve.grpc.chat";
        reference.conversation_id = request->user();
        reference.turn_id = turn_id;
        reference.parent_policy =
            qai_forge::ConversationParentPolicy::Latest;
        options.conversation = std::move(reference);
    }
    return options;
}

} // namespace

// Note on session lifecycle: gRPC calls in this service are stateless
// single-shot completions — BuildRequest() above never creates a
// ChatCompletionStore/ResponseStore entry, so there is no per-session
// backend state (e.g. LiteRT-LM KV cache session) to release here. The
// clearSession() cleanup added to ChatCompletionStore::deleteSession() /
// ResponseStore::deleteCascade() (see those files) does not apply to this
// transport for that reason — not an oversight.
grpc::Status ChatServiceImpl::CreateChatCompletion(
    grpc::ServerContext* context,
    const qai::forge::v1::ChatCompletionRequest* request,
    qai::forge::v1::ChatCompletionResponse* response) {
    CreateChatCompletionRequest dto = BuildRequest(request);
    qai_forge::GenerateOptions options = BuildOptions(request);

    try {
        auto pending = std::async(
            std::launch::async,
            [&dto, &options]() {
                return qai_forge::QaiForge::getInstance().generate(dto, options);
            });
        bool cancellation_requested = false;
        while (pending.wait_for(std::chrono::milliseconds(25)) !=
               std::future_status::ready) {
            if (context->IsCancelled() && !cancellation_requested) {
                qai_forge::QaiForge::getInstance().cancel(options.response_id);
                cancellation_requested = true;
            }
        }
        StandardResponse resp = pending.get();
        if (context->IsCancelled()) {
            return grpc::Status::CANCELLED;
        }
        response->set_id(resp.id);
        response->set_model(!resp.model.empty() ? resp.model : dto.model);
        response->set_content(resp.content.value_or(""));
        response->set_reasoning_content(resp.reasoning_content.value_or(""));
        response->set_finish_reason(resp.finish_reason);
        response->set_prompt_tokens(resp.prompt_tokens);
        response->set_completion_tokens(resp.completion_tokens);
        return grpc::Status::OK;
    } catch (const GenAIException& e) {
        return grpc::Status(grpc_util::MapErrorCode(e.code), e.message);
    }
}

grpc::Status ChatServiceImpl::CreateChatCompletionStream(
    grpc::ServerContext* context,
    const qai::forge::v1::ChatCompletionRequest* request,
    grpc::ServerWriter<qai::forge::v1::ChatCompletionChunk>* writer) {
    CreateChatCompletionRequest dto = BuildRequest(request);
    dto.stream = true;
    qai_forge::GenerateOptions options = BuildOptions(request);

    // Bridges QaiForge::generateStream()'s background-thread callbacks to
    // this handler thread, which owns `writer` and must call Write() itself
    // — grpc::ServerWriter is not safe to call cross-thread, unlike Drogon's
    // stream object used for the SSE path in ResponsesController.
    auto queue = std::make_shared<std::deque<qai::forge::v1::ChatCompletionChunk>>();
    auto mtx = std::make_shared<std::mutex>();
    auto cv = std::make_shared<std::condition_variable>();
    auto done = std::make_shared<bool>(false);
    auto final_status = std::make_shared<grpc::Status>(grpc::Status::OK);

    qai_forge::StreamCallbacks callbacks;
    callbacks.onToken = [=](const StreamChunk& sc) {
        qai::forge::v1::ChatCompletionChunk c;
        c.set_id(sc.id);
        c.set_model(sc.model);
        c.set_content_delta(sc.content_delta.value_or(""));
        c.set_reasoning_delta(sc.reasoning_content.value_or(""));
        c.set_finish_reason(sc.finish_reason.value_or(""));
        c.set_role(sc.role.value_or(""));
        {
            std::lock_guard<std::mutex> lock(*mtx);
            queue->push_back(std::move(c));
        }
        cv->notify_one();
    };
    callbacks.onComplete = [=](const StandardResponse&) {
        std::lock_guard<std::mutex> lock(*mtx);
        *done = true;
        cv->notify_one();
    };
    callbacks.onError = [=](const GenAIException& e) {
        std::lock_guard<std::mutex> lock(*mtx);
        *final_status = grpc::Status(grpc_util::MapErrorCode(e.code), e.message);
        *done = true;
        cv->notify_one();
    };
    callbacks.onCancelled = [=]() {
        std::lock_guard<std::mutex> lock(*mtx);
        *final_status = grpc::Status::CANCELLED;
        *done = true;
        cv->notify_one();
    };

    try {
        qai_forge::QaiForge::getInstance().generateStream(
            dto, std::move(callbacks), options);
    } catch (const GenAIException& e) {
        return grpc::Status(grpc_util::MapErrorCode(e.code), e.message);
    }

    // Drain the queue on THIS (handler) thread — Write() must be called here.
    while (true) {
        std::unique_lock<std::mutex> lock(*mtx);
        cv->wait_for(lock, std::chrono::milliseconds(25), [&] {
            return !queue->empty() || *done;
        });
        if (context->IsCancelled()) {
            lock.unlock();
            qai_forge::QaiForge::getInstance().cancel(options.response_id);
            return grpc::Status::CANCELLED;
        }
        while (!queue->empty()) {
            auto chunk = std::move(queue->front());
            queue->pop_front();
            lock.unlock();
            if (context->IsCancelled() || !writer->Write(chunk)) {
                qai_forge::QaiForge::getInstance().cancel(options.response_id);
                return grpc::Status::CANCELLED;
            }
            lock.lock();
        }
        if (*done && queue->empty()) {
            break;
        }
    }
    return *final_status;
}
