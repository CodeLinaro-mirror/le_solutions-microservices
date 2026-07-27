// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "ResponseStore.h"

#include <functional>
#include <string>

class ResponsesCompactionService {
public:
    using SummaryGenerator =
        std::function<std::string(const std::string& prompt,
                                  int max_output_tokens)>;

    struct CompactBranchResult {
        bool ok = false;
        bool compacted = false;
        int http_status = 200;
        std::string error_message;
        CompactionSummary summary;
    };

    /**
     * @brief Return the process-local compaction service.
     */
    static ResponsesCompactionService& getInstance();

    /**
     * @brief Compact older completed turns on a branch and store the summary.
     * @detail The caller supplies summary generation. This keeps POST/scheduler
     *         wiring out of the store-level foundation phase.
     */
    CompactBranchResult compactBranch(
        const std::string& branch_head_response_id,
        const std::string& model,
        const SummaryGenerator& generate_summary);

private:
    ResponsesCompactionService() = default;
    ResponsesCompactionService(const ResponsesCompactionService&) = delete;
    ResponsesCompactionService& operator=(const ResponsesCompactionService&) =
        delete;
};
