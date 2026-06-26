-- Install pgvector extension (fails fast if not available in the PostgreSQL instance)
CREATE EXTENSION IF NOT EXISTS vector;

-- Vector store registry: one row per named store
CREATE TABLE IF NOT EXISTS vector_stores (
    id TEXT PRIMARY KEY,                       -- e.g. "vs_hospital_records" or "vs_codebase"
    name TEXT NOT NULL,
    embedding_model TEXT NOT NULL,             -- e.g. "nomic-embed-text-v1" or "bge-m3"
    embedding_dim INTEGER NOT NULL,            -- e.g. 768 or 1024
    hnsw_m INTEGER NOT NULL DEFAULT 16,        -- HNSW graph connectivity parameter
    hnsw_ef_construction INTEGER NOT NULL DEFAULT 64, -- Beam width during build
    hnsw_ef_search INTEGER NOT NULL DEFAULT 40, -- Beam width during query session
    status TEXT NOT NULL DEFAULT 'active',     -- 'active' | 'inactive' | 'degraded'
    metadata JSONB NOT NULL DEFAULT '{}',      -- Custom store-level metadata
    created_at BIGINT NOT NULL                 -- Unix epoch timestamp in milliseconds
);

-- Documents: one row per text chunk with its embedding vector
-- Designed for 768-dim (nomic-embed-text-v1). Can be dynamically altered if needed.
CREATE TABLE IF NOT EXISTS vector_store_documents (
    id BIGSERIAL PRIMARY KEY,
    vector_store_id TEXT NOT NULL REFERENCES vector_stores(id) ON DELETE CASCADE,
    file_id TEXT,                              -- Optional grouping/source file identifier
    text_chunk TEXT NOT NULL,
    metadata JSONB NOT NULL DEFAULT '{}',      -- Key-value attributes (e.g. {"patient_id": "p001"})
    embedding vector(768),                     -- Confirmed 768-dim for nomic-embed
    created_at BIGINT NOT NULL,                -- Unix epoch timestamp in milliseconds
    deleted_at BIGINT                          -- Soft delete timestamp; NULL represents active
);
