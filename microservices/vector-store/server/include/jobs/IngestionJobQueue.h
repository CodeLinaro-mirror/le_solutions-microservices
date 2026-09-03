//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include "db/VectorStoreRepository.h"
#include "embedding/EmbeddingClient.h"
#include "VectorStoreDTOs.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <functional>

namespace jobs {

/**
 * @brief Async background ingestion job worker thread pool.
 * Prevents HTTP request timeouts during large file corpus indexing.
 */
class IngestionJobQueue {
public:
    IngestionJobQueue(
        db::VectorStoreRepository& repo,
        embedding::EmbeddingClient& embed_client,
        int32_t num_workers = 2,
        int32_t batch_size = 32
    );
    ~IngestionJobQueue();

    // Start background threads
    void start();

    // Stop background threads
    void stop();

    /**
     * @brief Enqueues a job for async processing.
     * @param job_id The tracking ID created in database.
     * @param store_id Destination vector store.
     * @param file_id Source file grouping identifier.
     * @param chunks Text chunks to embed and insert.
     */
    void enqueueJob(
        const std::string& job_id,
        const std::string& store_id,
        const std::string& file_id,
        std::vector<store::DocumentChunk> chunks
    );

private:
    struct QueuedJob {
        std::string job_id;
        std::string store_id;
        std::string file_id;
        std::vector<store::DocumentChunk> chunks;
    };

    db::VectorStoreRepository& repo_;
    embedding::EmbeddingClient& embed_client_;
    int32_t num_workers_;
    int32_t batch_size_;

    std::queue<QueuedJob> queue_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> is_running_;

    void worker_thread();
    void process_job(const QueuedJob& job);
};

} // namespace jobs
