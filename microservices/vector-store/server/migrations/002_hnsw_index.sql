-- HNSW index for fast Approximate Nearest Neighbor (ANN) search
-- Using vector_ip_ops (Inner Product) since nomic-embed-text-v1 outputs L2-normalized vectors.
-- Inner Product is faster and mathematically identical to Cosine Similarity for normalized vectors.
-- Note: CONCURRENTLY is omitted here because migrations run inside a transaction block.
CREATE INDEX IF NOT EXISTS idx_vsd_embedding_hnsw
    ON vector_store_documents
    USING hnsw (embedding vector_ip_ops)
    WITH (m = 16, ef_construction = 64);
