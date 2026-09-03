//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include <drogon/drogon.h>
#include "store/VectorStoreManager.h"
#include <iostream>
#include <cstdlib>

int32_t main() {
    std::cout << "[VectorStoreMain] Starting Vector Store Platform Service..." << std::endl;

    // 1. Bootstrapping singleton manager (running db pool connects, schema migrations, starting worker threads)
    try {
        store::VectorStoreManager::getInstance().initialize();
    } catch (const std::exception& e) {
        std::cerr << "[VectorStoreMain] Fatal error during platform initialization: " << e.what() << std::endl;
        return 1;
    }

    // 2. Resolve port binding from environment variables
    const char* port_env = std::getenv("VECTOR_STORE_PORT");
    int32_t port = port_env ? std::atoi(port_env) : 9005;

    // 3. Configure Drogon HTTP Server
    std::cout << "[VectorStoreMain] Binding HTTP listener on 0.0.0.0:" << port << "..." << std::endl;
    drogon::app().addListener("0.0.0.0", port);

    // Set standard threading model
    drogon::app().setThreadNum(4);

    // Set maximum client request body size to 20 MB to support large vector ingestion payloads
    drogon::app().setClientMaxBodySize(20 * 1024 * 1024);

    // Enable Global CORS (Cross-Origin Resource Sharing) support to allow browser-based Swagger UI testing
    drogon::app().registerPreRoutingAdvice([](const drogon::HttpRequestPtr &req, drogon::AdviceCallback &&hcb, drogon::AdviceChainCallback &&acb) {
        if (req->method() == drogon::HttpMethod::Options) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->addHeader("Access-Control-Allow-Origin", "*");
            resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, X-Requested-With");
            resp->addHeader("Access-Control-Max-Age", "86400");
            hcb(resp);
            return;
        }
        acb();
    });

    drogon::app().registerPostHandlingAdvice([](const drogon::HttpRequestPtr &req, const drogon::HttpResponsePtr &resp) {
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept, X-Requested-With");
    });

    // 4. Log when the event loop becomes active
    drogon::app().registerBeginningAdvice([]() {
        std::cout << "[VectorStoreMain] Drogon HTTP runtime loop is active. Ready to serve requests." << std::endl;
    });

    // 5. Run Drogon Event Loop (blocks main thread until SIGTERM/SIGINT)
    drogon::app().run();

    // 6. Graceful shutdown — called after run() returns
    std::cout << "[VectorStoreMain] Shutting down Drogon server. Halting background thread pools..." << std::endl;
    store::VectorStoreManager::getInstance().shutdown();

    std::cout << "[VectorStoreMain] Service halted cleanly." << std::endl;
    return 0;
}
