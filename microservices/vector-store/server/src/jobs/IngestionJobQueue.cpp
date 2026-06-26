//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "jobs/IngestionJobQueue.h"
#include <iostream>

namespace jobs {

IngestionJobQueue::IngestionJobQueue(
    db::VectorStoreRepository& repo,
    embedding::EmbeddingClient& embed_client,
    int32_t num_workers,
    int32_t batch_size
) : repo_(repo),
    embed_client_(embed_client),
    num_workers_(num_workers),
    batch_size_(batch_size),
    is_running_(false) {}

IngestionJobQueue::~IngestionJobQueue() {
    stop();
}

void IngestionJobQueue::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_) return;

    is_running_ = true;
    for (int32_t i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&IngestionJobQueue::worker_thread, this);
    }
    std::cout << "[IngestionJobQueue] Started " << num_workers_ << " background ingestion threads." << std::endl;
}

void IngestionJobQueue::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!is_running_) return;
        is_running_ = false;
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    std::cout << "[IngestionJobQueue] Stopped background workers cleanly." << std::endl;
}

void IngestionJobQueue::enqueueJob(
    const std::string& job_id,
    const std::string& store_id,
    const std::string& file_id,
    std::vector<store::DocumentChunk> chunks
) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(QueuedJob{job_id, store_id, file_id, std::move(chunks)});
    }
    cv_.notify_one();
}

void IngestionJobQueue::worker_thread() {
    while (is_running_) {
        QueuedJob job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return !queue_.empty() || !is_running_; });

            if (!is_running_ && queue_.empty()) {
                return;
            }

            job = std::move(queue_.front());
            queue_.pop();
        }

        try {
            process_job(job);
        } catch (const std::exception& e) {
            std::cerr << "[IngestionJobQueue] Critical error processing job " << job.job_id << ": " << e.what() << std::endl;
            try {
                repo_.failJob(job.job_id, std::string("Unhandled worker exception: ") + e.what());
            } catch (...) {}
        }
    }
}

void IngestionJobQueue::process_job(const QueuedJob& job) {
    std::cout << "[IngestionJobQueue] Background worker thread starting processing for job_id: " << job.job_id << std::endl;

    // 1. Fetch store metadata to validate embedding model constraints
    store::VectorStore store;
    try {
        store = repo_.getStore(job.store_id);
    } catch (const std::exception& e) {
        repo_.failJob(job.job_id, std::string("Target vector store not found: ") + e.what());
        return;
    }

    int32_t total = job.chunks.size();
    int32_t processed = 0;
    int32_t failed = 0;

    std::vector<store::DocumentChunk> batch_chunks;
    std::vector<std::string> batch_texts;

    for (int32_t i = 0; i < total; ++i) {
        batch_chunks.push_back(job.chunks[i]);
        batch_texts.push_back(job.chunks[i].text);

        // Process when batch size is reached or on the final iteration
        if (batch_chunks.size() == static_cast<size_t>(batch_size_) || i == total - 1) {
            try {
                // Fetch embeddings from the service
                auto embeddings = embed_client_.embedBatch(batch_texts);

                // Validation contract: Assert returned vector dimension matches database contract
                for (size_t k = 0; k < embeddings.size(); ++k) {
                    if (static_cast<int32_t>(embeddings[k].size()) != store.embedding_dim) {
                        std::string err_msg = "Dimension mismatch error: Embedding model returned " +
                                              std::to_string(embeddings[k].size()) + " dimensions, but vector store '" +
                                              store.id + "' requires " + std::to_string(store.embedding_dim) + ".";
                        std::cerr << "[IngestionJobQueue] " << err_msg << " Ingestion cancelled to prevent corruption." << std::endl;
                        throw std::runtime_error(err_msg);
                    }
                }

                // Batch insert chunks + vectors into Pgvector database
                repo_.insertDocuments(store.id, job.file_id, batch_chunks, embeddings);

                processed += batch_chunks.size();
                repo_.updateJobProgress(job.job_id, batch_chunks.size(), 0);

            } catch (const std::exception& e) {
                std::cerr << "[IngestionJobQueue] Batch processing failure in job " << job.job_id << ": " << e.what() << std::endl;
                failed += batch_chunks.size();
                repo_.updateJobProgress(job.job_id, 0, batch_chunks.size());

                // If there's a dimension mismatch contract or circuit breaker open, we fail the entire job immediately
                std::string err_str(e.what());
                if (err_str.find("Dimension mismatch") != std::string::npos ||
                    err_str.find("circuit breaker") != std::string::npos) {
                    repo_.failJob(job.job_id, std::string("Job aborted due to fatal error: ") + e.what());
                    return;
                }
            }

            batch_chunks.clear();
            batch_texts.clear();
        }
    }

    if (failed == total) {
        repo_.failJob(job.job_id, "All document batches failed to ingest.");
    } else if (failed > 0) {
        repo_.failJob(job.job_id, "Completed with partial errors. Total: " + std::to_string(total) +
                                  ", Processed: " + std::to_string(processed) + ", Failed: " + std::to_string(failed));
    } else {
        repo_.completeJob(job.job_id);
        std::cout << "[IngestionJobQueue] Successfully completed async ingestion job: " << job.job_id << std::endl;
    }
}

} // namespace jobs
