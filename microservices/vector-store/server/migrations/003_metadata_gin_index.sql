-- GIN (Generalized Inverted Index) on JSONB metadata column
-- This enables extremely fast metadata filtering during vector search, e.g.:
-- WHERE metadata @> '{"patient_id": "p001"}'
CREATE INDEX IF NOT EXISTS idx_vsd_metadata_gin
    ON vector_store_documents
    USING gin (metadata);
