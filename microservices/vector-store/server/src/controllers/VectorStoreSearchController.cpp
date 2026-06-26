//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "controllers/VectorStoreSearchController.h"

namespace controllers {

void VectorStoreSearchController::searchStore(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        json body = json::parse(req->body());
        store::SearchRequest s_req = body.get<store::SearchRequest>();

        auto s_res = manager_.searchStore(id, s_req);

        json j_res;
        store::to_json(j_res, s_res);

        resp->setBody(j_res.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    callback(resp);
}

} // namespace controllers
