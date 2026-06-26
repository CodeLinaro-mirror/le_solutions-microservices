//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "controllers/HealthController.h"
#include <fstream>
#include <sstream>
#include <iomanip>

namespace controllers {

void HealthController::getHealth(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    json body = json::object();
    body["status"] = "healthy";

    // 1. Check database connection health status
    std::string db_status = "healthy";
    try {
        auto list = manager_.repository().listStores();
    } catch (const std::exception& e) {
        db_status = "unhealthy: " + std::string(e.what());
        body["status"] = "unhealthy";
    }

    body["dependencies"] = {
        {"postgresql", db_status},
        {"embedding_service", manager_.embedding_client().isCircuitOpen() ? "circuit_open" : "healthy"},
        {"embedding_failures", manager_.embedding_client().consecutiveFailures()}
    };

    // 2. Fetch container RAM usage from cgroups
    std::string ram_usage_str = "0 MB";
    try {
        std::ifstream f("/sys/fs/cgroup/memory.current");
        if (!f.is_open()) {
            f.open("/sys/fs/cgroup/memory/memory.usage_in_bytes");
        }
        if (f.is_open()) {
            long long bytes = 0;
            f >> bytes;
            f.close();
            double mib = bytes / (1024.0 * 1024.0);
            std::stringstream ss;
            ss << std::fixed << std::setprecision(2) << mib << " MiB";
            ram_usage_str = ss.str();
        }
    } catch (...) {}

    // 3. Fetch database size metrics
    std::string db_size = "0 kB";
    std::string table_size = "0 kB";
    std::string index_size = "0 kB";
    try {
        auto stats = manager_.repository().getDatabaseStats();
        db_size = stats[0];
        table_size = stats[1];
        index_size = stats[2];
    } catch (...) {}

    body["stats"] = {
        {"container_ram_usage", ram_usage_str},
        {"postgres_db_total_size", db_size},
        {"documents_table_total_size", table_size},
        {"hnsw_index_size", index_size}
    };

    resp->setBody(body.dump());
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void HealthController::getStoreHealth(
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

        json body = json::object();
        body["store"] = s_json;
        body["status"] = "active";
        body["hnsw_index_status"] = "valid";
        body["embedding_service_circuit"] =
            manager_.embedding_client().isCircuitOpen() ? "open" : "closed";

        resp->setBody(body.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k404NotFound);
    }

    callback(resp);
}

} // namespace controllers
