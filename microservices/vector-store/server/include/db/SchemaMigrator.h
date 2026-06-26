//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include "db/PgConnectionPool.h"
#include <string>

namespace db {

/**
 * @brief Handles running numbered SQL migrations at service startup.
 * Ensures schema versioning is robust, transactional, and fails fast.
 */
class SchemaMigrator {
public:
    SchemaMigrator(PgConnectionPool& pool, const std::string& migrations_dir);

    /**
     * @brief Executes migrations. Fails fast if any migration fails or pgvector is missing.
     * @return Current schema version after applying migrations.
     */
    int32_t migrate();

private:
    PgConnectionPool& pool_;
    std::string migrations_dir_;

    void create_migration_table_if_not_exists(pqxx::connection& conn);
    int32_t get_current_version(pqxx::connection& conn);
    void update_version(pqxx::connection& conn, int32_t version, const std::string& filename);
    void execute_migration_file(pqxx::connection& conn, const std::string& file_path);
};

} // namespace db
