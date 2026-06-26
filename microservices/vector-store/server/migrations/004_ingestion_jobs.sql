-- Async ingestion job tracking table
CREATE TABLE IF NOT EXISTS ingestion_jobs (
    id TEXT PRIMARY KEY,                       -- e.g. "job_abc123"
    vector_store_id TEXT NOT NULL REFERENCES vector_stores(id) ON DELETE CASCADE,
    status TEXT NOT NULL DEFAULT 'processing', -- 'processing' | 'completed' | 'failed'
    total_chunks INTEGER NOT NULL DEFAULT 0,
    processed_chunks INTEGER NOT NULL DEFAULT 0,
    failed_chunks INTEGER NOT NULL DEFAULT 0,
    error_message TEXT,
    created_at BIGINT NOT NULL,                -- Unix epoch timestamp in milliseconds
    completed_at BIGINT                        -- Unix epoch timestamp in milliseconds
);
