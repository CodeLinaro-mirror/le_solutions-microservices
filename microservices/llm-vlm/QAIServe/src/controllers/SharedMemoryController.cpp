// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "controllers/SharedMemoryController.h"
#include "shm/SharedMemoryManager.h"

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

HttpResponsePtr jsonResp(const json& body, HttpStatusCode code = k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

HttpResponsePtr errorResp(const std::string& msg, HttpStatusCode code) {
    return jsonResp({{"error", msg}}, code);
}

// Every route requires QAISERVE_ALLOW_CLIENT_SHM=1. Returns true if allowed;
// sends a 403 and returns false otherwise.
bool checkAllowed(std::function<void(const HttpResponsePtr&)>& callback) {
    if (!SharedMemoryManager::getInstance().allowClientShm()) {
        callback(errorResp(
            "Client shared memory is disabled. Set QAISERVE_ALLOW_CLIENT_SHM=1 to enable.",
            k403Forbidden));
        return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/systemsharedmemory/region/{name}/register
// ─────────────────────────────────────────────────────────────────────────────
void SharedMemoryController::registerRegion(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& name)
{
    if (!checkAllowed(callback)) return;

    json body;
    try {
        body = json::parse(req->getBody());
    } catch (...) {
        callback(errorResp("Invalid JSON body.", k400BadRequest));
        return;
    }

    if (!body.contains("key") || !body["key"].is_string() || body["key"].get<std::string>().empty()) {
        callback(errorResp("'key' is required.", k400BadRequest));
        return;
    }
    if (!body.contains("byte_size")) {
        callback(errorResp("'byte_size' is required.", k400BadRequest));
        return;
    }

    std::string shm_key = body["key"].get<std::string>();
    size_t offset;
    size_t byte_size;
    try {
        offset    = static_cast<size_t>(body.value("offset", static_cast<uint64_t>(0)));
        byte_size = static_cast<size_t>(body.at("byte_size").get<uint64_t>());
    } catch (const std::exception& e) {
        callback(errorResp(
            std::string("'offset'/'byte_size' must be non-negative integers: ") + e.what(),
            k400BadRequest));
        return;
    }

    try {
        SharedMemoryManager::getInstance().registerRegion(name, shm_key, offset, byte_size);
    } catch (const std::exception& e) {
        std::string msg = e.what();
        HttpStatusCode code = msg.find("already registered") != std::string::npos
                                   ? k409Conflict : k400BadRequest;
        callback(errorResp(msg, code));
        return;
    }

    callback(jsonResp({
        {"name",      name},
        {"key",       shm_key},
        {"offset",    offset},
        {"byte_size", byte_size},
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/systemsharedmemory/region/{name}/status
// ─────────────────────────────────────────────────────────────────────────────
void SharedMemoryController::statusOne(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& name)
{
    if (!checkAllowed(callback)) return;
    try {
        callback(jsonResp(SharedMemoryManager::getInstance().getStatus(name)));
    } catch (const std::exception& e) {
        callback(errorResp(e.what(), k404NotFound));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v2/systemsharedmemory/status
// ─────────────────────────────────────────────────────────────────────────────
void SharedMemoryController::statusAll(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    if (!checkAllowed(callback)) return;
    callback(jsonResp(SharedMemoryManager::getInstance().getStatus("")));
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/systemsharedmemory/region/{name}/unregister
// ─────────────────────────────────────────────────────────────────────────────
void SharedMemoryController::unregisterOne(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& name)
{
    if (!checkAllowed(callback)) return;
    try {
        SharedMemoryManager::getInstance().unregister(name);
    } catch (const std::exception& e) {
        callback(errorResp(e.what(), k404NotFound));
        return;
    }
    callback(jsonResp({{"unregistered", name}}));
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v2/systemsharedmemory/unregister
// ─────────────────────────────────────────────────────────────────────────────
void SharedMemoryController::unregisterAll(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback)
{
    if (!checkAllowed(callback)) return;
    SharedMemoryManager::getInstance().unregister("");
    callback(jsonResp({{"unregistered", "all"}}));
}
