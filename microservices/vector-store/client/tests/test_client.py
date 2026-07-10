# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Unit tests for the Vector Store Python Client and main orchestrator application.
"""

import sys
import os
import pytest
import requests
from unittest.mock import MagicMock, patch

# Ensure the source directory is in the sys.path for testing
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "../src")))

from client import VectorStoreClient, SearchResponse, IngestionResponse
from main import VectorStoreClientApplication


def test_chunk_text_sliding_window_basic():
    """Test standard sliding window chunking with whitespace alignment."""
    app = VectorStoreClientApplication()
    text = "one two three four five six seven eight nine ten"

    # Chunk size 15, overlap 5.
    chunks = app.chunk_text_sliding_window(text, 15, 5)
    assert len(chunks) > 0
    # Make sure words are preserved
    assert "one" in chunks[0]
    assert "ten" in chunks[-1]


def test_chunk_text_sliding_window_empty():
    """Test chunking with empty or whitespace-only strings."""
    app = VectorStoreClientApplication()
    assert app.chunk_text_sliding_window("", 10, 2) == []
    assert app.chunk_text_sliding_window("   ", 10, 2) == []


@patch("requests.post")
def test_create_store_success(mock_post):
    """Test successful collection registration."""
    mock_response = MagicMock()
    mock_response.json.return_value = {"status": "active", "id": "test_store"}
    mock_response.raise_for_status = MagicMock()
    mock_post.return_value = mock_response

    client = VectorStoreClient("http://localhost:9005")
    success = client.create_store("test_store", "My Collection", "nomic-embed-text", 512)

    assert success is True
    mock_post.assert_called_once()


@patch("requests.post")
def test_create_store_failure(mock_post):
    """Test registration failure handling."""
    mock_post.side_effect = Exception("Connection refused")

    client = VectorStoreClient("http://localhost:9005")
    success = client.create_store("test_store", "My Collection", "nomic-embed-text", 512)

    assert success is False


@patch("requests.post")
def test_get_embeddings_success(mock_post):
    """Test successful embedding generation via sidecar."""
    mock_response = MagicMock()
    mock_response.json.return_value = {
        "data": [
            {
                "embedding": [0.1, 0.2, 0.3]
            }
        ]
    }
    mock_response.raise_for_status = MagicMock()
    mock_post.return_value = mock_response

    client = VectorStoreClient("http://localhost:9005")
    embs = client.get_embeddings(["test text"])

    assert len(embs) == 1
    assert embs[0] == [0.1, 0.2, 0.3]


@patch("requests.post")
def test_insert_with_vectors_batched(mock_post):
    """Test direct vector insertion behaves with optimal batches."""
    mock_response = MagicMock()
    mock_response.json.return_value = {"status": "completed"}
    mock_response.raise_for_status = MagicMock()
    mock_post.return_value = mock_response

    client = VectorStoreClient("http://localhost:9005")

    # 150 texts (will split into 2 batches of 100 and 50)
    texts = [f"chunk_{i}" for i in range(150)]
    embeddings = [[0.1] * 512 for _ in range(150)]

    success = client.insert_with_vectors("test_store", "test.txt", texts, embeddings)

    assert success is True
    assert mock_post.call_count == 2


@patch("requests.post")
def test_search_results_filtering(mock_post):
    """Test semantic search retrieval parsing and filtering."""
    mock_response = MagicMock()
    mock_response.json.return_value = {
        "retrieval_status": "ok",
        "best_score_found": 0.85,
        "embedding_latency_ms": 10,
        "search_latency_ms": 5,
        "results": [
            {
                "id": "doc_1",
                "score": 0.85,
                "text": "Matched excerpt description",
                "metadata": {"source": "test.txt"},
                "file_id": "test.txt"
            }
        ]
    }
    mock_response.raise_for_status = MagicMock()
    mock_post.return_value = mock_response

    client = VectorStoreClient("http://localhost:9005")
    res = client.search("test_store", "query text", top_k=5)

    assert res.retrieval_status == "ok"
    assert len(res.results) == 1
    assert res.results[0].id == "doc_1"
    assert res.results[0].score == 0.85
