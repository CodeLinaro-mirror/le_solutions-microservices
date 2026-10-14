# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Object-Oriented Python Client Application that orchestrates the Vector Store pipeline.

Mirrors the exact orchestration, error handling, path resolution, and console formatting
of the C++ client application.
"""

import sys
import os
import json
import time
from typing import List, Dict, Any, Optional
from pypdf import PdfReader
from client import VectorStoreClient, SearchResponse


class VectorStoreClientApplication:
    """
    Orchestration application class that handles config loading, path translation,
    native PDF loading, text chunking, embedding generation, database indexing,
    and semantic querying.
    """

    def __init__(self):
        self.base_url = "http://localhost:9005"
        self.input_arg = str()
        self.search_query = ""
        self.store_id = str()
        self.embed_duration_ms = 0.0
        self.ingest_duration_ms = 0.0
        self.embedding_port = 9090
        self.profiling_enabled = False
        self.is_file = False
        self.inputs: List[str] = []
        self.host_prefix = str()
        self.start_total = time.perf_counter()

    def run(self, args: List[str]) -> int:
        """
        Orchestrates and executes the entire workflow.

        Args:
            args: Command line arguments.

        Returns:
            int: Exit code (0 for success, 1 for failure)
        """
        print("🚀 Initializing Integrated Python Client (OOP Style)...")

        # 1. Load configuration from env and fallback JSON
        self.load_config()

        # 2. Parse overriding command-line arguments
        self.parse_args(args)

        print(f"🔗 Vector Store API Gateway: {self.base_url}")

        # 3. Process the input file or prompt
        if not self.process_input():
            return 1

        # 4. Initialize our Vector Store client helper SDK
        vs_client = VectorStoreClient(self.base_url)
        vs_client.set_embedding_port(self.embedding_port)
        store_id = self.store_id

        # 5. Ingestion Workflow (if input is a file)
        if self.is_file:
            # Generate vector embeddings from NPU
            print(f"\n🧠 Sending prompt(s) to 'genai_service_t2e' container (Port {self.embedding_port}) for embedding generation...")

            start_embed = time.perf_counter()
            embeddings = vs_client.get_embeddings(self.inputs)
            end_embed = time.perf_counter()
            self.embed_duration_ms = (end_embed - start_embed) * 1000.0

            if len(embeddings) != len(self.inputs):
                print(
                    f"❌ Failed to generate embeddings for all input chunks. "
                    f"(Expected: {len(self.inputs)}, Generated: {len(embeddings)})",
                    file=sys.stderr
                )
                return 1
            print(f"✅ Successfully generated {len(embeddings)} float vector(s) from NPU/TPU accelerator.")
            print(f"   Vector dimensions: {0 if not embeddings else len(embeddings[0])}")

            # Register store/collection
            print(f"\n🛠️ Registering Vector Store Collection '{store_id}'...")
            store_ok = vs_client.create_store(store_id, "Integrated Store", "nomic-embed-text", 512)
            if not store_ok:
                print("⚠️ Store creation returned false. It may already exist; continuing...")
            else:
                print("✅ Collection registered successfully.")

            # Ingest to database
            print("\n📥 Saving text chunks + generated float vectors into 'vector-store' database...")
            start_ingest = time.perf_counter()
            save_ok = vs_client.insert_with_vectors(store_id, self.input_arg, self.inputs, embeddings)
            end_ingest = time.perf_counter()
            self.ingest_duration_ms = (end_ingest - start_ingest) * 1000.0

            if not save_ok:
                print("❌ Direct database insertion failed!", file=sys.stderr)
                return 1
            print("🎉 Saved! Document chunks and high-dimensional embeddings indexed in pgvector.")

            if not self.search_query:
                self.search_query = self.inputs[0]

        # 6. Search Query Workflow
        start_search = time.perf_counter()
        print(f"\n🔍 Executing semantic query: \"{self.search_query}\"...")
        search_res = vs_client.search(store_id, self.search_query, 5, 0.0)
        end_search = time.perf_counter()
        client_search_duration_ms = (end_search - start_search) * 1000.0

        print(f"\n✨ SEMANTIC RETRIEVAL RESULTS ({search_res.retrieval_status})")
        print("=============================================================================")

        has_high_similarity = False
        if search_res.results and search_res.results[0].score >= 0.60:
            has_high_similarity = True

        if not search_res.results or not has_high_similarity:
            print("I couldn't find any relevant information in the documents to answer your question.")
            print("Please try rephrasing your question or check if the relevant documents have been ingested.")
        else:
            count = 0
            for r in search_res.results:
                if r.score < 0.60:
                    continue
                count += 1
                print(f"{count}. ID: {r.id} [Score: {r.score:.6f}]")
                print(f"   Excerpt: \"{r.text}\"")
                print(f"   Source : {r.file_id}")
                print("-----------------------------------------------------------------------------")

        end_total = time.perf_counter()
        total_duration_ms = (end_total - self.start_total) * 1000.0

        # Calculate dynamic network delay
        server_processing_time = float(search_res.embedding_latency_ms + search_res.search_latency_ms)
        port_transfer_ms = client_search_duration_ms - server_processing_time
        if port_transfer_ms < 0.0:
            port_transfer_ms = 0.0

        # Print profiling results conditionally
        if self.profiling_enabled:
            print("\n=============================================================================")
            print("⏱️  PERFORMANCE TIMING & PORT TRANSFER MEASUREMENTS")
            print("=============================================================================")
            if self.is_file:
                print("⚡ Ingestion Workflow:")
                print(f"   - Client-side Embedding Generation: {self.embed_duration_ms:.2f} ms")
                print(f"   - Server-side Ingestion/Database Insert: {self.ingest_duration_ms:.2f} ms")
            else:
                print("⚡ Ingestion Workflow: [SKIPPED - PURE SEARCH MODE]")
            print("⚡ Search Query Workflow:")
            print(f"   - On-Device Query Embedding Generation: {search_res.embedding_latency_ms} ms")
            print(f"   - pgvector Database HNSW Search Query : {search_res.search_latency_ms} ms")
            print(f"   - Port Loopback/ADB Transfer Overhead : {port_transfer_ms:.3f} ms (network delay)")
            print(f"   - Client-Side Search Round-Trip (RTT) : {client_search_duration_ms:.3f} ms")
            print(f"⚡ Total End-to-End Execution Latency    : {total_duration_ms:.2f} ms")
            print("=============================================================================")

            stats = vs_client.get_health_stats()
            if stats:
                print("\n=============================================================================")
                print("📊 ON-DEVICE RESOURCE PROFILING & DISK ALLOCATION")
                print("=============================================================================")
                print("💾 Database disk allocations:")
                print(f"   - Total Postgres Database Size    : {self.format_size_string(stats.get('postgres_db_total_size', 'N/A'))}")
                print(f"   - documents Table total size      : {self.format_size_string(stats.get('documents_table_total_size', 'N/A'))}")
                print(f"   - idx_vsd_embedding_hnsw index size: {self.format_size_string(stats.get('hnsw_index_size', 'N/A'))}")
                print("🧠 Active Container Memory Usage:")
                print(f"   - vector-store-platform RAM usage : {stats.get('container_ram_usage', 'N/A')}")
                print("=============================================================================")

        print("\n🌟 Integrated Python Client completed successfully!")
        return 0

    def format_size_string(self, raw_size: str) -> str:
        """Parses and formats size strings to MB if greater than 1024 kB."""
        if not raw_size or raw_size == "N/A":
            return raw_size

        parts = raw_size.split()
        if len(parts) == 2:
            try:
                val = float(parts[0])
                unit = parts[1].strip().lower()
                if unit in ("kb", "kilobytes") and val >= 1024.0:
                    return f"{val / 1024.0:.2f} MB"
                elif unit == "bytes" and val >= 1024.0 * 1024.0:
                    return f"{val / (1024.0 * 1024.0):.2f} MB"
            except ValueError:
                pass
        return raw_size

    def load_config(self) -> None:
        """Loads default configurations from environment variables and JSON fallback."""
        # 1. Load Dotenv first
        self.load_dot_env()

        # 2. Resolve environment variables
        url_env = os.getenv("VECTOR_STORE_URL")
        port_env = os.getenv("VECTOR_STORE_SERVER_PORT")
        embed_env = os.getenv("VECTOR_STORE_EMBEDDING_PORT")
        input_file_env = os.getenv("VECTOR_STORE_INPUT_FILE")
        input_prompt_env = os.getenv("VECTOR_STORE_INPUT_PROMPT")
        profiling_env = os.getenv("VECTOR_STORE_PROFILING_ENABLED")
        store_id_env = os.getenv("VECTOR_STORE_STORE_ID")

        if url_env:
            self.base_url = url_env
        elif port_env:
            self.base_url = f"http://localhost:{port_env}"

        if store_id_env:
            self.store_id = store_id_env

        if embed_env:
            try:
                self.embedding_port = int(embed_env)
            except ValueError:
                pass

        if input_file_env and len(input_file_env) > 0:
            self.input_arg = input_file_env
        elif input_prompt_env:
            self.input_arg = input_prompt_env

        if input_prompt_env:
            self.search_query = input_prompt_env

        if profiling_env:
            self.profiling_enabled = profiling_env.lower() in ("true", "1", "yes", "on")

        # 3. Fall back to JSON config
        config_path = self.resolve_config_path()
        if os.path.exists(config_path):
            try:
                with open(config_path, "r", encoding="utf-8") as f:
                    config = json.load(f)
                print(f"⚙️  Loaded configuration from {config_path}!")

                if not url_env and not port_env:
                    if "server_port" in config:
                        port_val = config["server_port"]
                        self.base_url = f"http://localhost:{port_val}"
                if not embed_env and "embedding_port" in config:
                    self.embedding_port = int(config["embedding_port"])
                if not input_file_env and not input_prompt_env:
                    if "input_file" in config and config["input_file"]:
                        self.input_arg = config["input_file"]
                    elif "input_prompt" in config:
                        self.input_arg = config["input_prompt"]
                if not input_prompt_env and "input_prompt" in config:
                    self.search_query = config["input_prompt"]
                if not store_id_env and "store_id" in config:
                    self.store_id = config["store_id"]
                if not profiling_env and "profiling_enabled" in config:
                    prof_node = config["profiling_enabled"]
                    if isinstance(prof_node, bool):
                        self.profiling_enabled = prof_node
                    elif isinstance(prof_node, str):
                        self.profiling_enabled = prof_node.lower() in ("true", "1", "yes", "on")
                    elif isinstance(prof_node, (int, float)):
                        self.profiling_enabled = bool(prof_node)
            except Exception as e:
                print(f"⚠️  Warning: Failed to parse vector_store_client_config.json: {e}", file=sys.stderr)

        # 4. Dynamically auto-derive host_prefix from input_file if not explicitly set
        if not self.host_prefix:
            input_file_path = ""
            if os.path.exists(config_path):
                try:
                    with open(config_path, "r", encoding="utf-8") as f:
                        config = json.load(f)
                        if "input_file" in config and config["input_file"]:
                            input_file_path = config["input_file"]
                except Exception:
                    pass
            if not input_file_path and self.input_arg:
                input_file_path = self.input_arg

            if input_file_path and ("/" in input_file_path or "\\" in input_file_path):
                dirname = os.path.dirname(input_file_path)
                if dirname and dirname != "/data" and dirname != "/data/":
                    self.host_prefix = dirname if dirname.endswith("/") else dirname + "/"
                    print(f"📂 Automatically derived host_prefix from input path: '{self.host_prefix}'")

        if not self.base_url:
            print("❌ Fatal Error: Client base URL is not defined!", file=sys.stderr)
            raise RuntimeError("Configuration error: client base URL is missing.")

    def load_dot_env(self) -> None:
        """Loads environment variables from .env if present."""
        candidates = ["/data/.env", ".env", "/app/.env", "../.env"]
        for path in candidates:
            if os.path.exists(path):
                print(f"⚙️  Loading variables from .env file: {path}")
                try:
                    with open(path, "r", encoding="utf-8") as f:
                        for line in f:
                            line = line.strip()
                            if not line or line.startswith("#"):
                                continue
                            if "=" not in line:
                                continue
                            parts = line.split("=", 1)
                            key = parts[0].strip()
                            val = parts[1].strip()
                            # Strip quotes
                            if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
                                val = val[1:-1]
                            os.environ[key] = val
                    break
                except Exception:
                    pass

    def resolve_config_path(self) -> str:
        """Resolves the client configuration path preference."""
        candidates = [
            "/data/client/vector_store_client_config.json",
            "/data/vector_store_client/vector_store_client_config.json",
            "/data/vector_store_client_config.json",
            "vector_store_client_config.json",
            "vector_store_client/vector_store_client_config.json"
        ]
        for path in candidates:
            if os.path.exists(path):
                return path
        return "vector_store_client_config.json"

    def parse_args(self, args: List[str]) -> None:
        """Parses overriding arguments."""
        if len(args) > 1:
            self.base_url = args[1]
        if len(args) > 2:
            self.input_arg = args[2]
        if len(args) > 3:
            self.search_query = args[3]

    def process_input(self) -> bool:
        """Processes the input parameter as a file or query prompt."""
        host_prefix = self.host_prefix
        if host_prefix and self.input_arg.startswith(host_prefix):
            self.input_arg = "/data/" + self.input_arg[len(host_prefix):]

        is_pdf = self.input_arg.lower().endswith(".pdf")

        if is_pdf:
            self.is_file = True
            print(f"📄 Binary PDF File Detected: {self.input_arg}")
            print("⚙️  Extracting raw text from PDF natively in-memory using pypdf...")

            if not os.path.exists(self.input_arg):
                print(f"❌ Error: PDF file does not exist: {self.input_arg}", file=sys.stderr)
                return False

            try:
                reader = PdfReader(self.input_arg)
                num_pages = len(reader.pages)
                print(f"ℹ️  PDF contains {num_pages} page(s). Extracting...")
                file_content = ""
                for page in reader.pages:
                    file_content += (page.extract_text() or "") + "\n"
                print(f"✅ Successfully extracted PDF text. Length: {len(file_content)} characters.")

                self.inputs = self.chunk_text_sliding_window(file_content, 512, 64)
                print(f"✂️ Automated chunking generated {len(self.inputs)} word-boundary aligned chunk(s).")

                if not self.inputs:
                    print("❌ Error: No valid text chunks could be parsed or generated.", file=sys.stderr)
                    return False
                return True
            except Exception as e:
                print(f"❌ Native pypdf failed to load PDF document: {e}", file=sys.stderr)
                return False

        is_json_file = self.input_arg.lower().endswith(".json")
        if os.path.exists(self.input_arg):
            self.is_file = True
            print(f"📂 Input Document Database File: {self.input_arg}")

            if is_json_file:
                try:
                    with open(self.input_arg, "r", encoding="utf-8") as file:
                        root = json.load(file)
                    if "data" in root and isinstance(root["data"], list):
                        for item in root["data"]:
                            if "input" in item and isinstance(item["input"], str):
                                self.inputs.append(item["input"])
                    print(f"✅ Parsed {len(self.inputs)} pre-chunked segments from JSON.")
                except Exception as e:
                    print(f"❌ JSON Parsing failed: {e}", file=sys.stderr)
                    return False
            else:
                try:
                    with open(self.input_arg, "r", encoding="utf-8") as file:
                        file_content = file.read()
                    print(f"✅ Successfully read file content. Length: {len(file_content)} characters.")

                    self.inputs = self.chunk_text_sliding_window(file_content, 512, 64)
                    print(f"✂️ Automated chunking generated {len(self.inputs)} word-boundary aligned chunk(s).")
                except Exception as e:
                    print(f"❌ Failed to read input file: {e}", file=sys.stderr)
                    return False

            if not self.inputs:
                print("❌ Error: No valid text chunks could be parsed or generated.", file=sys.stderr)
                return False
        else:
            print(f"💬 Raw Text Search Query Detected: \"{self.input_arg}\"")
            self.search_query = self.input_arg
            print("✅ Treating as direct search query (skipping database insertion pipeline).")

        return True

    def chunk_text_sliding_window(self, text: str, chunk_size: int, chunk_overlap: int) -> List[str]:
        """Word-boundary aligned sliding window text chunker matching C++ logic."""
        if not text.strip():
            return []

        words = text.split()
        chunks: List[str] = []
        current_chunk_words: List[str] = []
        current_len = 0

        for w in words:
            word_len = len(w) if not current_chunk_words else len(w) + 1

            if current_len + word_len > chunk_size and current_chunk_words:
                chunks.append(" ".join(current_chunk_words))

                overlap_words: List[str] = []
                overlap_len = 0
                for prev_w in reversed(current_chunk_words):
                    w_len = len(prev_w) if not overlap_words else len(prev_w) + 1
                    if overlap_len + w_len > chunk_overlap:
                        break
                     # insert at front
                    overlap_words.insert(0, prev_w)
                    overlap_len += w_len

                current_chunk_words = overlap_words
                current_len = overlap_len
                word_len = len(w) if not current_chunk_words else len(w) + 1

            current_chunk_words.append(w)
            current_len += len(w) if len(current_chunk_words) == 1 else len(w) + 1

        if current_chunk_words:
            chunks.append(" ".join(current_chunk_words))

        return chunks


if __name__ == "__main__":
    app = VectorStoreClientApplication()
    sys.exit(app.run(sys.argv))
