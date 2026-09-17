// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SharedMemoryController — client-facing system shared memory REST API
//
// Mirrors Triton Inference Server's /v2/systemsharedmemory extension. All
// endpoints require QAISERVE_ALLOW_CLIENT_SHM=1 (unset ⇒ 403 on every route)
// — see shm/SharedMemoryManager.h for why this is gated separately from
// QAISERVE_ADMIN_TOKEN.
//
// Routes:
//   GET  /v2/systemsharedmemory/status                — status, all regions
//   GET  /v2/systemsharedmemory/region/{name}/status   — status, one region
//   POST /v2/systemsharedmemory/region/{name}/register — register a region
//   POST /v2/systemsharedmemory/region/{name}/unregister — unregister one
//   POST /v2/systemsharedmemory/unregister             — unregister all
// ─────────────────────────────────────────────────────────────────────────────

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

class SharedMemoryController : public drogon::HttpController<SharedMemoryController> {
public:
    METHOD_LIST_BEGIN
        // GET /v2/systemsharedmemory/status — status of all registered regions
        ADD_METHOD_TO(SharedMemoryController::statusAll,
                      "/v2/systemsharedmemory/status", Get, Options);

        // POST /v2/systemsharedmemory/unregister — unregister all regions
        ADD_METHOD_TO(SharedMemoryController::unregisterAll,
                      "/v2/systemsharedmemory/unregister", Post, Options);

        // GET /v2/systemsharedmemory/region/{name}/status — status of one region
        ADD_METHOD_TO(SharedMemoryController::statusOne,
                      "/v2/systemsharedmemory/region/{1}/status", Get, Options);

        // POST /v2/systemsharedmemory/region/{name}/register — register a region
        ADD_METHOD_TO(SharedMemoryController::registerRegion,
                      "/v2/systemsharedmemory/region/{1}/register", Post, Options);

        // POST /v2/systemsharedmemory/region/{name}/unregister — unregister one region
        ADD_METHOD_TO(SharedMemoryController::unregisterOne,
                      "/v2/systemsharedmemory/region/{1}/unregister", Post, Options);
    METHOD_LIST_END

    /**
     * POST /v2/systemsharedmemory/region/{name}/register
     *
     * Body (JSON):
     *   {
     *     "key":       "/qaiserve_input_0",  // required — shm_open() key
     *     "offset":    0,                     // optional, default 0
     *     "byte_size": 602112                 // required
     *   }
     *
     * Opens the POSIX named shared memory segment `key` and mmaps
     * [offset, offset+byte_size). Returns 200 on success, 400 if the key
     * can't be opened or the range is out of bounds for the segment, 409 if
     * `name` is already registered.
     */
    void registerRegion(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& name);

    /**
     * GET /v2/systemsharedmemory/region/{name}/status
     *
     * Returns { "name", "key", "offset", "byte_size" } for the named region.
     * 404 if not registered.
     */
    void statusOne(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback,
                   const std::string& name);

    /**
     * GET /v2/systemsharedmemory/status
     *
     * Returns { "regions": [ {"name","key","offset","byte_size"}, ... ] }.
     */
    void statusAll(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * POST /v2/systemsharedmemory/region/{name}/unregister
     *
     * Unregisters the named region. The underlying mapping is unmapped once
     * any in-flight /infer or /generate request still reading it finishes.
     * 404 if not registered.
     */
    void unregisterOne(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback,
                       const std::string& name);

    /**
     * POST /v2/systemsharedmemory/unregister
     *
     * Unregisters all regions.
     */
    void unregisterAll(const HttpRequestPtr& req,
                       std::function<void(const HttpResponsePtr&)>&& callback);
};
