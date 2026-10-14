# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Standalone Python Client SDK wrapper for the Vector Store Platform.

This client helper manages store registration, semantic similarity search, and automated
document ingestion pipelines, mirroring the exact architecture and API design of the
C++ client helper.
"""

from dataclasses import dataclass, field
import os
import sys
from typing import List, Dict, Any, Optional
import requests


@dataclass
class SearchResult:
    """Represents a single query result from the vector store search."""
    id: str
    score: float
    text: str
    metadata: Dict[str, Any]
    file_id: str


@dataclass
class SearchResponse:
    """Represents the complete semantic search query response."""
    results: List[SearchResult] = field(default_factory=list)
    retrieval_status: str = "no_match"
    best_score_found: float = 0.0
    embedding_latency_ms: int = 0
    search_latency_ms: int = 0


@dataclass
class IngestionResponse:
    """Represents the status response of an asynchronous document ingestion job."""
    job_id: str = ""
    status: str = ""
    error_message: str = ""


class VectorStoreClient:
    """
    Client wrapper SDK to interact with the Vector Store API and on-device sidecars.
    """

    def __init__(self, base_url: str):
        """
        Initialize the Vector Store Client.

        Args:
            base_url: The HTTP base URL of the running Vector Store service (e.g. "http://localhost:9005")
        """
        self.base_url = base_url.rstrip("/")
        self.embedding_port = 9090

        # Read environment override for embedding port
        env_port = os.getenv("VECTOR_STORE_EMBEDDING_PORT")
        if env_port:
            try:
                self.embedding_port = int(env_port)
            except ValueError:
                pass

    def set_embedding_port(self, port: int) -> None:
        """
        Configures the port number of the local text-embeddings container.

        Args:
            port: Port number (e.g., 9090)
        """
        self.embedding_port = port

    def create_store(self, store_id: str, name: str, model: str, dimension: int) -> bool:
        """
        Registers a new Vector Store collection.

        Args:
            store_id: Collection identifier
            name: Friendly display name
            model: Embedding model name
            dimension: Embedding dimensionality

        Returns:
            bool: True if created successfully or already exists, False otherwise.
        """
        url = f"{self.base_url}/v1/vector_stores"
        payload = {
            "id": store_id,
            "name": name,
            "embedding_model": model,
            "embedding_dim": dimension
        }

        try:
            response = self._perform_post(url, payload)
            data = response.json()
            status = data.get("status", "")
            return status == "active" or "id" in data
        except requests.exceptions.HTTPError as e:
            try:
                # Intercept safe "already exists" or duplicate key database warnings in the JSON body
                error_data = e.response.json()
                err_body = error_data.get("error", "")
                if "already exists" in err_body or "duplicate" in err_body:
                    return False
            except Exception:
                pass

            # Print failure message if it is not a safe, expected "already exists" duplicate store warning
            err_msg = str(e)
            print(f"[VectorStoreClient] Failed to create store: {err_msg}", file=sys.stderr)
            return False
        except Exception as e:
            err_msg = str(e)
            if "already exists" not in err_msg and "duplicate key" not in err_msg:
                print(f"[VectorStoreClient] Failed to create store: {err_msg}", file=sys.stderr)
            return False

    def ingest_documents(self, store_id: str, file_id: str, texts: List[str]) -> IngestionResponse:
        """
        Triggers asynchronous ingestion of text chunks.

        Args:
            store_id: Target collection ID
            file_id: Optional grouping source identifier
            texts: Chunks of text to ingest

        Returns:
            IngestionResponse
        """
        url = f"{self.base_url}/v1/vector_stores/{store_id}/files"
        chunks = [
            {
                "text": t,
                "metadata": {"source": "vscode_python_client"}
            } for t in texts
        ]
        payload = {
            "file_id": file_id,
            "chunks": chunks
        }

        try:
            response = self._perform_post(url, payload)
            data = response.json()
            return IngestionResponse(
                job_id=data.get("job_id", ""),
                status=data.get("status", ""),
                error_message=data.get("error_message", "")
            )
        except Exception as e:
            return IngestionResponse(
                status="failed",
                error_message=str(e)
            )

    def get_embeddings(self, texts: List[str]) -> List[List[float]]:
        """
        Generates vector embeddings from the external genai_service_t2e container (on Port 9090).

        Args:
            texts: Text chunks to compile embeddings for

        Returns:
            List[List[float]]: List of high-dimensional float vectors
        """
        results = []
        url = f"http://localhost:{self.embedding_port}/v1/embeddings"

        for t in texts:
            payload = {
                "input": t
            }

            try:
                response = self._perform_post(url, payload)
                data = response.json()

                if "data" in data and isinstance(data["data"], list) and len(data["data"]) > 0:
                    emb_item = data["data"][0]
                    if "embedding" in emb_item:
                        emb_val = emb_item["embedding"]
                        if isinstance(emb_val, list):
                            results.append(emb_val)
                        elif isinstance(emb_val, dict) and "actual_instance" in emb_val:
                            results.append(emb_val["actual_instance"])
            except Exception as e:
                print(f"[VectorStoreClient] Failed to get embedding for text: {e}", file=sys.stderr)

        return results

    def insert_with_vectors(self, store_id: str, file_id: str, texts: List[str], embeddings: List[List[float]]) -> bool:
        """
        Directly saves document chunks and pre-computed float embeddings in optimal batches of 100
        to prevent HTTP connection timeouts and server memory exhaustion.

        Args:
            store_id: Target collection ID
            file_id: Optional grouping source identifier
            texts: List of document chunks
            embeddings: List of pre-computed high-dimensional embeddings

        Returns:
            bool: True if ingestion completed successfully, False otherwise
        """
        if not texts:
            return True

        url = f"{self.base_url}/v1/vector_stores/{store_id}/insert_with_vectors"
        batch_size = 100

        for i in range(0, len(texts), batch_size):
            end = min(i + batch_size, len(texts))
            batch_texts = texts[i:end]
            batch_embeddings = embeddings[i:end]

            chunks = [
                {
                    "text": t,
                    "metadata": {"source": "vscode_python_client_direct"}
                } for t in batch_texts
            ]
            payload = {
                "file_id": file_id,
                "chunks": chunks,
                "embeddings": batch_embeddings
            }

            try:
                print(f"   ↳ Ingesting vector batch ({i + 1} to {end} of {len(texts)})...")
                response = self._perform_post(url, payload)
                data = response.json()
                if data.get("status", "") != "completed":
                    return False
            except Exception as e:
                print(f"[VectorStoreClient] Direct vector insertion failed at batch {i // batch_size + 1}: {e}", file=sys.stderr)
                return False

        return True

    def poll_job_status(self, store_id: str, job_id: str) -> str:
        """
        Polls an asynchronous ingestion job's status.

        Args:
            store_id: Collection ID
            job_id: Job identifier to poll

        Returns:
            str: Status string ("completed", "processing", or "failed")
        """
        url = f"{self.base_url}/v1/vector_stores/{store_id}/files/{job_id}"
        try:
            response = self._perform_get(url)
            data = response.json()
            return data.get("status", "processing")
        except Exception as e:
            print(f"[VectorStoreClient] Failed to poll job status: {e}", file=sys.stderr)
            return "failed"

    def search(self, store_id: str, query: str, top_k: int = 5, score_threshold: float = 0.0) -> SearchResponse:
        """
        Performs a semantic similarity search query.

        Args:
            store_id: Target collection ID
            query: Pure text search query
            top_k: Number of top results to retrieve
            score_threshold: Minimum score filter

        Returns:
            SearchResponse
        """
        url = f"{self.base_url}/v1/vector_stores/{store_id}/search"
        payload = {
            "query": query,
            "top_k": top_k,
            "score_threshold": score_threshold,
            "filter": {}
        }

        try:
            response = self._perform_post(url, payload)
            data = response.json()

            if "error" in data:
                print(f"[VectorStoreClient] Search error: {data['error']}", file=sys.stderr)
                return SearchResponse()

            search_res = SearchResponse(
                retrieval_status=data.get("retrieval_status", "no_match"),
                best_score_found=data.get("best_score_found", 0.0),
                embedding_latency_ms=data.get("embedding_latency_ms", 0),
                search_latency_ms=data.get("search_latency_ms", 0)
            )

            if "results" in data and isinstance(data["results"], list):
                for item in data["results"]:
                    search_res.results.append(SearchResult(
                        id=item.get("id", ""),
                        score=item.get("score", 0.0),
                        text=item.get("text", ""),
                        metadata=item.get("metadata", {}),
                        file_id=item.get("file_id", "")
                    ))

            return search_res

        except Exception as e:
            print(f"[VectorStoreClient] Connection failure or parsing failed in search: {e}", file=sys.stderr)
            return SearchResponse()

    def get_health_stats(self) -> Dict[str, Any]:
        """
        Fetches real-time system and database statistics from the health check endpoint.

        Returns:
            Dict[str, Any]: System stats if successful, empty dict otherwise.
        """
        url = f"{self.base_url}/v1/health"
        try:
            response = self._perform_get(url)
            data = response.json()
            return data.get("stats", {})
        except Exception:
            return {}

    def _perform_post(self, url: str, payload: Dict[str, Any]) -> requests.Response:
        """Helper to perform HTTP POST with a 60s timeout."""
        response = requests.post(url, json=payload, timeout=60.0)
        response.raise_for_status()
        return response

    def _perform_get(self, url: str) -> requests.Response:
        """Helper to perform HTTP GET with a 60s timeout."""
        response = requests.get(url, timeout=60.0)
        response.raise_for_status()
        return response
