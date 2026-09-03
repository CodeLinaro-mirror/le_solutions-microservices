//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "db/PgConnectionPool.h"
#include <chrono>
#include <stdexcept>
#include <iostream>

namespace db {

PgConnectionPool::PgConnectionPool(const std::string& conn_str, int32_t pool_size)
    : conn_str_(conn_str), pool_size_(pool_size), is_shutting_down_(false) {

    std::lock_guard<std::mutex> lock(mutex_);
    for (int32_t i = 0; i < pool_size_; ++i) {
        try {
            auto conn = std::make_unique<pqxx::connection>(conn_str_);
            if (conn->is_open()) {
                pool_.push(std::move(conn));
            } else {
                throw std::runtime_error("Database connection was created but is not open.");
            }
        } catch (const std::exception& e) {
            std::cerr << "[PgConnectionPool] Initial database connection error: " << e.what() << std::endl;
            // Throw if we cannot establish any connections at start
            if (i == 0) {
                throw;
            }
        }
    }
}

PgConnectionPool::~PgConnectionPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_shutting_down_ = true;
    while (!pool_.empty()) {
        auto conn = std::move(pool_.front());
        pool_.pop();
        if (conn && conn->is_open()) {
            try {
                conn->close();
            } catch (...) {}
        }
    }
    cv_.notify_all();
}

PgConnectionPool::Guard PgConnectionPool::acquire(int32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (pool_.empty() && !is_shutting_down_) {
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            throw std::runtime_error("Database connection pool exhausted. Timeout reached while waiting for a connection.");
        }
    }

    if (is_shutting_down_) {
        throw std::runtime_error("Database connection pool is shutting down.");
    }

    auto conn = std::move(pool_.front());
    pool_.pop();

    // Verify connection is healthy before handing it out; otherwise, establish a new one on the fly.
    try {
        if (!conn || !conn->is_open()) {
            conn = std::make_unique<pqxx::connection>(conn_str_);
        }
    } catch (const std::exception& e) {
        std::cerr << "[PgConnectionPool] Reconnection failed during acquire: " << e.what() << std::endl;
        throw std::runtime_error(std::string("Database reconnection failed: ") + e.what());
    }

    return Guard(*this, std::move(conn));
}

void PgConnectionPool::release(std::unique_ptr<pqxx::connection> conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutting_down_) {
        if (conn && conn->is_open()) {
            conn->close();
        }
        return;
    }

    if (conn && conn->is_open()) {
        pool_.push(std::move(conn));
        cv_.notify_one();
    }
}

int32_t PgConnectionPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_size_;
}

int32_t PgConnectionPool::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.size();
}

// ================= Guard Implementation =================

PgConnectionPool::Guard::Guard(PgConnectionPool& pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(&pool), conn_(std::move(conn)) {}

PgConnectionPool::Guard::~Guard() {
    if (pool_ && conn_) {
        pool_->release(std::move(conn_));
    }
}

pqxx::connection& PgConnectionPool::Guard::conn() {
    if (!conn_) {
        throw std::runtime_error("Attempted to access connection from null/moved RAII Guard.");
    }
    return *conn_;
}

PgConnectionPool::Guard::Guard(Guard&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) {
    other.pool_ = nullptr;
}

PgConnectionPool::Guard& PgConnectionPool::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        if (pool_ && conn_) {
            pool_->release(std::move(conn_));
        }
        pool_ = other.pool_;
        conn_ = std::move(other.conn_);
        other.pool_ = nullptr;
    }
    return *this;
}

} // namespace db
