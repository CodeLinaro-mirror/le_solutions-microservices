//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include <pqxx/pqxx>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>

namespace db {

/**
 * @brief Thread-safe PostgreSQL connection pool using libpqxx.
 * Enforces resource constraints and provides clean RAII-based access to connections.
 */
class PgConnectionPool {
public:
    PgConnectionPool(const std::string& conn_str, int32_t pool_size);
    ~PgConnectionPool();

    // Prevent copying or moving connection pool to protect locks and connections
    PgConnectionPool(const PgConnectionPool&) = delete;
    PgConnectionPool& operator=(const PgConnectionPool&) = delete;
    PgConnectionPool(PgConnectionPool&&) = delete;
    PgConnectionPool& operator=(PgConnectionPool&&) = delete;

    /**
     * @brief RAII guard to safely borrow a connection and automatically return it upon destruction.
     */
    class Guard {
    public:
        Guard(PgConnectionPool& pool, std::unique_ptr<pqxx::connection> conn);
        ~Guard();

        // Access the underlying pqxx connection
        pqxx::connection& conn();

        // Prevent copying
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        // Allow moving
        Guard(Guard&&) noexcept;
        Guard& operator=(Guard&&) noexcept;

    private:
        PgConnectionPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    /**
     * @brief Borrows a connection from the pool. Blocks up to timeout_ms if pool is exhausted.
     * @throws std::runtime_error if pool is exhausted and timeout is reached.
     */
    Guard acquire(int32_t timeout_ms = 5000);

    /**
     * @brief Returns a connection back to the pool. Called automatically by Guard destructor.
     */
    void release(std::unique_ptr<pqxx::connection> conn);

    int32_t size() const;
    int32_t available() const;

private:
    std::string conn_str_;
    int32_t pool_size_;
    std::queue<std::unique_ptr<pqxx::connection>> pool_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool is_shutting_down_;
};

} // namespace db
