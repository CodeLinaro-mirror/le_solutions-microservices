# ==========================================
# STAGE 1: Build Environment (Natively on Host Platform)
# ==========================================
ARG BUILDARCH
FROM --platform=linux/${BUILDARCH} ubuntu:24.04 AS builder

# Declare BuildKit platform variables
ARG MAX_CPU_CORES_ARG

# Avoid interactive prompts during package installations
ENV DEBIAN_FRONTEND=noninteractive

# Configure dpkg multiarch, adjust apt sources to fetch amd64 from archive.ubuntu.com and arm64 from ports.ubuntu.com, and install cross-compilers
#
# Glossary of Stage 1 Build Dependencies:
#   - build-essential: Core toolchain (gcc, g++, make) required for local builds.
#   - crossbuild-essential-arm64: Native ARM64 cross-compiler for compilation targeting ARM64 platform.
#   - cmake: Cross-platform build system orchestrator.
#   - git: Source version control system.
#   - pkg-config: Package management tool to locate installed dependencies.
#   - libjsoncpp-dev:arm64: JSON parser and serializer utilized natively by Drogon.
#   - libpqxx-dev:arm64: Native PostgreSQL client C++ driver interface.
#   - libcurl4-openssl-dev:arm64: Client-side network transport transfer library.
#   - nlohmann-json3-dev: Modern header-only JSON parser and serializer.
#   - uuid-dev:arm64: Universally Unique Identifier library for generating transaction/doc IDs.
#   - zlib1g-dev:arm64: Compression and decompression shared headers.
#   - libbrotli-dev:arm64: Brotli high-compression algorithm headers.
#   - libsqlite3-dev:arm64: SQLite3 relational database development headers.
#   - libhiredis-dev:arm64: Redis standard key-value cache client library.
#   - libyaml-cpp-dev:arm64: YAML configuration parser library.
RUN dpkg --add-architecture arm64                                                               && \
    # Append Architectures: amd64 right after Signed-By in default ubuntu.sources using a physical newline inside sed
    sed -i 's|Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg|Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\nArchitectures: amd64|g' \
        /etc/apt/sources.list.d/ubuntu.sources                                                  && \
    # Create ports.sources specifically for arm64
    cp /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/ports.sources             && \
    sed -i 's|http://archive.ubuntu.com/ubuntu/|http://ports.ubuntu.com/ubuntu-ports/|g'           \
        /etc/apt/sources.list.d/ports.sources                                                   && \
    sed -i 's|http://security.ubuntu.com/ubuntu/|http://ports.ubuntu.com/ubuntu-ports/|g'          \
        /etc/apt/sources.list.d/ports.sources                                                   && \
    sed -i 's/Architectures: amd64/Architectures: arm64/g'                                         \
        /etc/apt/sources.list.d/ports.sources                                                   && \
    apt-get update && apt-get install -y --no-install-recommends                                   \
    build-essential                                                                                \
    crossbuild-essential-arm64                                                                     \
    cmake                                                                                          \
    git                                                                                            \
    pkg-config                                                                                     \
    libjsoncpp-dev:arm64                                                                           \
    libpqxx-dev:arm64                                                                              \
    libcurl4-openssl-dev:arm64                                                                     \
    nlohmann-json3-dev                                                                             \
    uuid-dev:arm64                                                                                 \
    zlib1g-dev:arm64                                                                               \
    libbrotli-dev:arm64                                                                            \
    libsqlite3-dev:arm64                                                                           \
    libhiredis-dev:arm64                                                                           \
    libyaml-cpp-dev:arm64                                                                       && \
    rm -rf /var/lib/apt/lists/*

# Clone, configure, compile, and install the high-performance Drogon Framework from source
# targeting the linux/arm64 architecture. Explicitly disables MySQL/MariaDB dependencies,
# and builds as a static library to allow complete standalone compilation of the final Vector Store binary.
RUN git config --global http.sslVerify false                                                    && \
    DROGON_SRC_DIR="/mnt/work"                                                                  && \
    git clone --depth 1 --recursive                                                                \
        https://github.com/drogonframework/drogon.git "${DROGON_SRC_DIR}"                       && \
    cd "${DROGON_SRC_DIR}"                                                                      && \
    mkdir build && cd build                                                                     && \
    cmake -DCMAKE_BUILD_TYPE=Release                                                               \
          -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc                                                 \
          -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++                                               \
          -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu                                            \
          -DBUILD_SHARED_LIBS=ON                                                                   \
          -DBUILD_MYSQL=OFF                                                                        \
          -DBUILD_SQLITE=ON                                                                        \
          -DBUILD_POSTGRESQL=ON                                                                    \
          -DBUILD_EXAMPLES=OFF                                                                     \
          -DBUILD_TESTING=OFF                                                                      \
          "${DROGON_SRC_DIR}"                                                                   && \
    make -j${MAX_CPU_CORES_ARG}                                                                 && \
    make install

# Set up build workspace
WORKDIR /build

# Copy only CMakeLists.txt to optimize layer caching for dependency configuration
COPY server/CMakeLists.txt ./

# Copy all source, header directories, and database migrations
COPY server/include/ ./include/
COPY server/src/ ./src/
COPY server/migrations/ ./migrations/

# Compile the application in Release mode using the ARM64 cross-compiler
RUN APP_SRC_DIR="/build"                                                                        && \
    mkdir build && cd build                                                                     && \
    export PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig                                 && \
    export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig                               && \
    cmake -DCMAKE_BUILD_TYPE=Release                                                               \
          -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc                                                 \
          -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++                                               \
          -DCMAKE_PREFIX_PATH=/usr/aarch64-linux-gnu                                               \
          "${APP_SRC_DIR}"                                                                      && \
    make -j${MAX_CPU_CORES_ARG}

ARG TARGETARCH

# ==========================================
# STAGE 2: Lightweight Runtime Environment (Target ARM64 Platform)
# ==========================================
FROM --platform=${TARGETARCH} ubuntu:24.04 AS runtime

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install only minimal runtime dependencies for the binary
#
# Glossary of Stage 2 Runtime Dependencies:
#   - libsqlite3-0: SQLite relational database runtime library (used statically by Drogon).
#   - libjsoncpp25: JsonCpp serialization parser runtime library.
#   - libpqxx-7.8: PostgreSQL client API driver runtime library.
#   - libcurl4: Libcurl network transfer runtime library.
#   - libuuid1: Universally Unique Identifier runtime library.
#   - zlib1g: Standard decompression runtime library.
#   - libyaml-cpp0.8: YAML configuration parser runtime library.
#   - libbrotli1: Brotli compression algorithm runtime library.
#   - libhiredis1.1.0: Redis standard key-value cache client runtime library.
#   - ca-certificates: Trustworthy root certificates for secure SSL connections.
#   - wget: Command-line utility to pull files and execute healthchecks.
RUN apt-get update && apt-get install -y --no-install-recommends                                   \
    libsqlite3-0                                                                                   \
    libjsoncpp25                                                                                   \
    libpqxx-7.8                                                                                    \
    libcurl4                                                                                       \
    libuuid1                                                                                       \
    zlib1g                                                                                         \
    libyaml-cpp0.8                                                                                 \
    libbrotli1                                                                                     \
    libhiredis1.1.0                                                                                \
    ca-certificates                                                                                \
    wget                                                                                        && \
    rm -rf /var/lib/apt/lists/*

# Set up runtime directory
WORKDIR /app

# Copy the compiled executable from the builder stage
COPY --from=builder /build/build/vector-store /app/server

# Copy the compiled shared libraries of Drogon and Trantor from the builder stage
COPY --from=builder /usr/aarch64-linux-gnu/lib/libdrogon.so* /usr/lib/
COPY --from=builder /usr/aarch64-linux-gnu/lib/libtrantor.so* /usr/lib/

# Copy SQL migrations from the builder stage into the app folder
COPY --from=builder /build/build/migrations /app/migrations

# Copy centralized configuration file (.env) into the runtime image
COPY .env /app/.env

# Define platform-wide default environment variables (can be overridden at runtime)
ENV VECTOR_STORE_PORT=9005
ENV VECTOR_STORE_PG_CONN=""
ENV VECTOR_STORE_PG_POOL_SIZE=10
ENV EMBEDDING_SERVICE_URL=""
ENV EMBEDDING_SERVICE_TIMEOUT_MS=10000
ENV EMBEDDING_CIRCUIT_BREAKER_THRESHOLD=5
ENV EMBEDDING_CIRCUIT_BREAKER_COOLDOWN_S=30
ENV VECTOR_STORE_INGEST_BATCH_SIZE=32
ENV VECTOR_STORE_INGEST_WORKERS=2
ENV VECTOR_STORE_MIGRATIONS_DIR=/app/migrations

# Expose the designated service port
EXPOSE 9005

# Define entrypoint
ENTRYPOINT ["/app/server"]
