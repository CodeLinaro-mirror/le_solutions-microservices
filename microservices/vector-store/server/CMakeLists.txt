cmake_minimum_required(VERSION 3.20)
project(vector-store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -- Dependencies ----------------------------------------------------------
find_package(Drogon REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)

# libpqxx ships only a pkg-config file on Ubuntu 24.04 (no CMake config)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQXX REQUIRED libpqxx)

# System UUID library for random UUID string generation on Linux
find_path(UUID_INCLUDE_DIR uuid/uuid.h)
find_library(UUID_LIBRARY uuid)
if(NOT UUID_LIBRARY)
    message(FATAL_ERROR "UUID library (uuid-dev on Ubuntu) not found but is required!")
endif()

# -- Header Directory ------------------------------------------------------
include_directories(include)

# -- Sources ---------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/controllers/VectorStoreController.cpp
    src/controllers/VectorStoreFilesController.cpp
    src/controllers/VectorStoreSearchController.cpp
    src/controllers/HealthController.cpp
    src/controllers/McpController.cpp
    src/store/VectorStoreManager.cpp
    src/db/PgConnectionPool.cpp
    src/db/SchemaMigrator.cpp
    src/db/VectorStoreRepository.cpp
    src/embedding/EmbeddingClient.cpp
    src/jobs/IngestionJobQueue.cpp
)

# -- Executable Build ------------------------------------------------------
add_executable(vector-store ${SOURCES})

# -- Library Linking -------------------------------------------------------
target_include_directories(vector-store PRIVATE ${LIBPQXX_INCLUDE_DIRS})

target_link_libraries(vector-store PRIVATE
    Drogon::Drogon
    ${LIBPQXX_LIBRARIES}
    CURL::libcurl
    nlohmann_json::nlohmann_json
    ${UUID_LIBRARY}
)

# -- Embed SQL migrations as runtime assets ---------------------------------
file(GLOB MIGRATION_FILES "migrations/*.sql")
foreach(migration_file ${MIGRATION_FILES})
    get_filename_component(migration_name ${migration_file} NAME)
    configure_file(${migration_file}
        ${CMAKE_CURRENT_BINARY_DIR}/migrations/${migration_name} COPYONLY)
endforeach()
