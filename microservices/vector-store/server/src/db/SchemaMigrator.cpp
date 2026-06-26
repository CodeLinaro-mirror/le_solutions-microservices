//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "db/SchemaMigrator.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <regex>

namespace db {

SchemaMigrator::SchemaMigrator(PgConnectionPool& pool, const std::string& migrations_dir)
    : pool_(pool), migrations_dir_(migrations_dir) {}

int32_t SchemaMigrator::migrate() {
    auto guard = pool_.acquire();
    auto& conn = guard.conn();

    // 1. Ensure the schema_migrations table exists
    create_migration_table_if_not_exists(conn);

    // 2. Read the current migration version
    int32_t current_version = get_current_version(conn);
    std::cout << "[SchemaMigrator] Current database schema version: " << current_version << std::endl;

    // 3. Scan the migrations directory for files matching 'XXX_*.sql'
    std::vector<std::pair<int32_t, std::filesystem::path>> migrations;

    if (!std::filesystem::exists(migrations_dir_)) {
        std::cerr << "[SchemaMigrator] Migrations directory does not exist: " << migrations_dir_ << std::endl;
        return current_version;
    }

    std::regex file_pattern(R"(^(\d+)_[a-zA-Z0-9_-]+\.sql$)");
    for (const auto& entry : std::filesystem::directory_iterator(migrations_dir_)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            std::smatch match;
            if (std::regex_match(filename, match, file_pattern)) {
                int32_t version = std::stoi(match[1].str());
                migrations.push_back({version, entry.path()});
            }
        }
    }

    // Sort migrations by version number sequentially
    std::sort(migrations.begin(), migrations.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    // 4. Run any pending migrations sequentially
    for (const auto& [version, path] : migrations) {
        if (version > current_version) {
            std::string filename = path.filename().string();
            std::cout << "[SchemaMigrator] Applying migration version " << version << ": " << filename << std::endl;

            try {
                // Run each migration inside a dedicated transaction scope (fails fast and rolls back on error)
                pqxx::work tx(conn);

                // Read and execute SQL file
                std::ifstream file(path);
                if (!file.is_open()) {
                    throw std::runtime_error("Could not open migration file: " + path.string());
                }
                std::stringstream buffer;
                buffer << file.rdbuf();
                std::string sql = buffer.str();

                // Execute SQL statements
                tx.exec(sql);

                // Update version registration in schema_migrations
                tx.exec_params(
                    "INSERT INTO schema_migrations (version, name, applied_at) VALUES ($1, $2, NOW())",
                    version,
                    filename
                );

                tx.commit();
                current_version = version;
                std::cout << "[SchemaMigrator] Successfully applied migration version " << version << std::endl;

            } catch (const std::exception& e) {
                std::cerr << "[SchemaMigrator] Critical Error: Migration version " << version
                          << " (" << filename << ") failed to apply! Details: " << e.what() << std::endl;
                throw; // Fail fast and exit service start
            }
        }
    }

    return current_version;
}

void SchemaMigrator::create_migration_table_if_not_exists(pqxx::connection& conn) {
    pqxx::work tx(conn);
    tx.exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "    version INTEGER PRIMARY KEY,"
        "    name TEXT NOT NULL,"
        "    applied_at TIMESTAMP WITH TIME ZONE DEFAULT NOW()"
        ")"
    );
    tx.commit();
}

int32_t SchemaMigrator::get_current_version(pqxx::connection& conn) {
    pqxx::nontransaction tx(conn);
    pqxx::result res = tx.exec("SELECT MAX(version) FROM schema_migrations");
    if (res.empty() || res[0][0].is_null()) {
        return 0;
    }
    return res[0][0].as<int32_t>();
}

} // namespace db
