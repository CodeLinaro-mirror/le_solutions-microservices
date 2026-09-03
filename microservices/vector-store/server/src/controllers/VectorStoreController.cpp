//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "controllers/VectorStoreController.h"
#include <iostream>

namespace controllers {

void VectorStoreController::createStore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        json body = json::parse(req->body());
        store::CreateStoreRequest s_req = body.get<store::CreateStoreRequest>();

        auto store = manager_.repository().createStore(s_req);

        json s_json;
        store::to_json(s_json, store);

        resp->setBody(s_json.dump());
        resp->setStatusCode(drogon::k201Created);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k400BadRequest);
    }

    callback(resp);
}

void VectorStoreController::listStores(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        auto stores = manager_.repository().listStores();

        json j_list = json::array();
        for (const auto& store : stores) {
            json s_json;
            store::to_json(s_json, store);
            j_list.push_back(s_json);
        }

        resp->setBody(json{{"data", j_list}}.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    callback(resp);
}

void VectorStoreController::getStore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        auto store = manager_.repository().getStore(id);

        json s_json;
        store::to_json(s_json, store);

        resp->setBody(s_json.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k404NotFound);
    }

    callback(resp);
}

void VectorStoreController::updateStore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        json body = json::parse(req->body());
        std::string name = body.value("name", "");
        json metadata = body.value("metadata", json::object());

        auto store = manager_.repository().updateStore(id, name, metadata);

        json s_json;
        store::to_json(s_json, store);

        resp->setBody(s_json.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        std::string err(e.what());
        if (err.find("not found") != std::string::npos) {
            resp->setBody(json{{"error", e.what()}}.dump());
            resp->setStatusCode(drogon::k404NotFound);
        } else {
            resp->setBody(json{{"error", e.what()}}.dump());
            resp->setStatusCode(drogon::k400BadRequest);
        }
    }

    callback(resp);
}

void VectorStoreController::deleteStore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        manager_.repository().deleteStore(id);
        resp->setBody(json{{"deleted", true}, {"id", id}}.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    callback(resp);
}

} // namespace controllers
