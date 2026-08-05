// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ws/WsProtocol.h"
#include <cstdlib>

namespace WsProtocol {

// ─────────────────────────────────────────────────────────────────────────────
// make_error
// ─────────────────────────────────────────────────────────────────────────────
json make_error(int status,
                const std::string& code,
                const std::string& message,
                const std::string& param) {
    json error_obj = {
        {"type",    (status >= 500) ? ERR_SERVER_ERROR : ERR_INVALID_REQUEST},
        {"code",    code},
        {"message", message}
    };
    if (!param.empty()) {
        error_obj["param"] = param;
    }

    return {
        {"type",   SERVER_ERROR},
        {"status", status},
        {"error",  error_obj}
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// connection_timeout_minutes
// ─────────────────────────────────────────────────────────────────────────────
int connection_timeout_minutes() {
    const char* env = std::getenv("RESPONSES_WS_TIMEOUT_MINUTES");
    if (env && env[0] != '\0') {
        try {
            int val = std::stoi(env);
            if (val > 0) return val;
        } catch (...) {}
    }
    return 10;  // default: 10 minutes
}

} // namespace WsProtocol
