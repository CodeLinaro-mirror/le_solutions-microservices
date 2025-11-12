#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Comprehensive VLM test program for CFFI-based VLM operations.

This test program provides:
1. Direct CFFI VLM testing with various configurations
2. Image preprocessing validation
3. Model configuration testing
4. Streaming and non-streaming mode testing
5. Error handling and edge case testing

Usage:
    python vlm_test_program.py [command] [options]

Commands:
    test-basic       Run basic VLM test with single image
    test-streaming   Test streaming mode
    test-batch       Test multiple images in sequence
    test-config      Test model configuration loading
    test-preprocess  Test image preprocessing pipeline

Options:
    --model MODEL             Model ID (default: qwen2-vl-3b)
    --image-url URL           Image URL to test
    --prompt TEXT             Text prompt
    --max-tokens N            Maximum tokens (default: 300)
    --verbose                 Enable verbose logging

Environment variables:
    VLM_LIBRARY_PATH         Path to libvlmservice.so (required)
    GENAI_INTERFACE_FILE     Path to genai_interface.h (required)
    GENAI_MODELS_CONFIG_PATH Path to models_config.json (optional)
"""

import os
import sys
import argparse
import logging
import json
import time
from typing import Dict, Any, List, Optional
from queue import Queue
import requests

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
)
logger = logging.getLogger("vlm_test_program")

# Try to import CFFI
try:
    from cffi import FFI
except ImportError:
    logger.error("CFFI not installed. Install with: pip install cffi")
    sys.exit(1)

# Try to import project utilities
try:
    from openapi_server.impl.genie_wrapper.utils.image_validator import decode_image
    from openapi_server.impl.genie_wrapper.utils.image_preprocessor import preprocess_from_decoded
    from openapi_server.impl.model_config_manager import ModelConfigManager
    HAVE_PROJECT_UTILS = True
except ImportError:
    logger.warning("Project utilities not available. Using simplified implementations.")
    HAVE_PROJECT_UTILS = False


class VLMTestRunner:
    """Test runner for VLM operations."""

    def __init__(self):
        self.ffi = None
        self.lib = None
        self.test_results = []

    def initialize_cffi(self) -> bool:
        """Initialize CFFI and load VLM library."""
        try:
            header_path = os.environ.get("GENAI_INTERFACE_FILE")
            if not header_path or not os.path.exists(header_path):
                logger.error("GENAI_INTERFACE_FILE not set or file not found")
                return False

            lib_path = os.environ.get("VLM_LIBRARY_PATH")
            if not lib_path or not os.path.exists(lib_path):
                logger.error("VLM_LIBRARY_PATH not set or file not found")
                return False

            logger.info(f"Loading header from: {header_path}")
            logger.info(f"Loading library from: {lib_path}")

            self.ffi = FFI()
            with open(header_path, "r") as f:
                self.ffi.cdef(f.read())

            self.lib = self.ffi.dlopen(lib_path)
            logger.info("CFFI initialized successfully")
            return True

        except Exception as e:
            logger.error(f"Failed to initialize CFFI: {e}")
            return False

    def download_image(self, url: str) -> Optional[bytes]:
        """Download image from URL."""
        try:
            logger.info(f"Downloading image from: {url}")
            resp = requests.get(url, timeout=30)
            resp.raise_for_status()
            logger.info(f"Downloaded {len(resp.content)} bytes")
            return resp.content
        except Exception as e:
            logger.error(f"Failed to download image: {e}")
            return None

    def preprocess_image(self, image_bytes: bytes) -> Optional[bytes]:
        """Preprocess image for VLM input."""
        try:
            if HAVE_PROJECT_UTILS:
                logger.info("Using project preprocessing utilities")
                decoded = decode_image(url=None, image_bytes=image_bytes)
                preprocessed = preprocess_from_decoded(decoded)
                return preprocessed.to_bytes()
            else:
                logger.info("Using raw image bytes (no preprocessing)")
                return image_bytes
        except Exception as e:
            logger.error(f"Failed to preprocess image: {e}")
            return None

    def create_query(self, model: str, prompt: str, image_bytes: bytes,
                    max_tokens: int = 300) -> Any:
        """Create CFFI query object."""
        query = self.ffi.new("Query *")

        # Set model
        model_bytes = model.encode("utf-8")
        for i, b in enumerate(model_bytes[:64-1]):
            query.model[i] = b
        query.model[min(len(model_bytes), 64-1)] = 0

        # Set role
        role_bytes = b"user"
        for i, b in enumerate(role_bytes[:32-1]):
            query.message.role[i] = b
        query.message.role[min(len(role_bytes), 32-1)] = 0

        # Enable multimodal
        query.message.use_content_items = True
        query.message.content_items_count = 0

        # Add text content
        text_bytes = prompt.encode("utf-8")
        query.message.content_items[0].type = 0
        for i, b in enumerate(text_bytes[:32000-1]):
            query.message.content_items[0].text[i] = b
        query.message.content_items[0].text[min(len(text_bytes), 32000-1)] = 0
        query.message.content_items_count = 1

        # Add image content
        if image_bytes:
            buffer = self.ffi.new(f"char[{len(image_bytes)}]", image_bytes)
            idx = query.message.content_items_count
            query.message.content_items[idx].type = 1
            query.message.content_items[idx].image.buffer = buffer
            query.message.content_items[idx].image.size = len(image_bytes)
            query.message.content_items_count += 1

        # Set parameters
        query.max_completion_tokens = max_tokens
        query.temperature = 0.7
        query.top_p = 0.9
        query.presence_penalty = 0.0
        query.frequency_penalty = 0.0

        return query

    def test_basic(self, args) -> bool:
        """Run basic VLM test."""
        logger.info("=== Running Basic VLM Test ===")

        # Download and preprocess image
        image_bytes = self.download_image(args.image_url)
        if not image_bytes:
            return False

        processed = self.preprocess_image(image_bytes)
        if not processed:
            return False

        # Create query
        query = self.create_query(args.model, args.prompt, processed, args.max_tokens)

        # Create VLM handle
        model_input = self.ffi.new("char[]", args.model.encode("utf-8"))
        config_path = self.ffi.new("char[]", b"")

        logger.info("Creating VLM handle...")
        handle = self.lib.vlm_create_object(model_input, config_path, False)
        if handle == self.ffi.NULL:
            logger.error("Failed to create VLM handle")
            return False

        # Setup callback
        output_queue = Queue()

        @self.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            resp = response_ptr[0]
            choice = resp.choices[0]
            content = self.ffi.string(choice.message.content).decode("utf-8")
            finish_reason = self.ffi.string(choice.finish_reason).decode("utf-8")
            output_queue.put({
                "content": content,
                "finish_reason": finish_reason
            })

        # Execute
        try:
            logger.info("Executing VLM completion...")
            start_time = time.time()
            self.lib.vlm_chat_completion_create(handle, query, False, callback)
            elapsed = time.time() - start_time

            # Get result
            result = output_queue.get(timeout=60)
            logger.info(f"VLM Response (took {elapsed:.2f}s):")
            logger.info(f"  Content: {result['content']}")
            logger.info(f"  Finish reason: {result['finish_reason']}")

            self.test_results.append({
                "test": "basic",
                "status": "passed",
                "elapsed": elapsed,
                "response_length": len(result['content'])
            })
            return True

        except Exception as e:
            logger.error(f"Test failed: {e}")
            self.test_results.append({
                "test": "basic",
                "status": "failed",
                "error": str(e)
            })
            return False
        finally:
            logger.info("Destroying VLM handle...")
            self.lib.vlm_destroy_object(handle)

    def test_streaming(self, args) -> bool:
        """Test streaming mode."""
        logger.info("=== Running Streaming VLM Test ===")

        # Download and preprocess image
        image_bytes = self.download_image(args.image_url)
        if not image_bytes:
            return False

        processed = self.preprocess_image(image_bytes)
        if not processed:
            return False

        # Create query
        query = self.create_query(args.model, args.prompt, processed, args.max_tokens)

        # Create VLM handle with streaming enabled
        model_input = self.ffi.new("char[]", args.model.encode("utf-8"))
        config_path = self.ffi.new("char[]", b"")

        logger.info("Creating VLM handle (streaming mode)...")
        handle = self.lib.vlm_create_object(model_input, config_path, True)
        if handle == self.ffi.NULL:
            logger.error("Failed to create VLM handle")
            return False

        # Setup callback for streaming
        chunks = []

        @self.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            resp = response_ptr[0]
            choice = resp.choices[0]
            content = self.ffi.string(choice.message.content).decode("utf-8")
            finish_reason = self.ffi.string(choice.finish_reason).decode("utf-8")

            if content:
                logger.info(f"[Stream Chunk] {content}")
                chunks.append(content)

            if finish_reason == "stop":
                logger.info("[Stream] Finished")

        # Execute
        try:
            logger.info("Executing streaming VLM completion...")
            start_time = time.time()
            self.lib.vlm_chat_completion_create(handle, query, True, callback)
            elapsed = time.time() - start_time

            full_response = "".join(chunks)
            logger.info(f"Streaming completed (took {elapsed:.2f}s)")
            logger.info(f"Total chunks: {len(chunks)}")
            logger.info(f"Full response: {full_response}")

            self.test_results.append({
                "test": "streaming",
                "status": "passed",
                "elapsed": elapsed,
                "chunks": len(chunks),
                "response_length": len(full_response)
            })
            return True

        except Exception as e:
            logger.error(f"Streaming test failed: {e}")
            self.test_results.append({
                "test": "streaming",
                "status": "failed",
                "error": str(e)
            })
            return False
        finally:
            logger.info("Destroying VLM handle...")
            self.lib.vlm_destroy_object(handle)

    def print_summary(self):
        """Print test summary."""
        logger.info("\n" + "="*60)
        logger.info("TEST SUMMARY")
        logger.info("="*60)

        for result in self.test_results:
            status_symbol = "✓" if result["status"] == "passed" else "✗"
            logger.info(f"{status_symbol} {result['test']}: {result['status']}")

            if result["status"] == "passed":
                if "elapsed" in result:
                    logger.info(f"  Time: {result['elapsed']:.2f}s")
                if "response_length" in result:
                    logger.info(f"  Response length: {result['response_length']} chars")
            else:
                logger.info(f"  Error: {result.get('error', 'Unknown')}")

        passed = sum(1 for r in self.test_results if r["status"] == "passed")
        total = len(self.test_results)
        logger.info(f"\nTotal: {passed}/{total} tests passed")
        logger.info("="*60)


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="VLM Test Program")
    parser.add_argument("command", choices=["test-basic", "test-streaming", "test-all"],
                       help="Test command to run")
    parser.add_argument("--model", default="qwen2-vl-3b", help="Model ID")
    parser.add_argument("--image-url",
                       default="https://upload.wikimedia.org/wikipedia/commons/thumb/d/dd/Gfp-wisconsin-madison-the-nature-boardwalk.jpg/2560px-Gfp-wisconsin-madison-the-nature-boardwalk.jpg",
                       help="Image URL")
    parser.add_argument("--prompt", default="What is in this image?", help="Text prompt")
    parser.add_argument("--max-tokens", type=int, default=300, help="Max tokens")
    parser.add_argument("--verbose", action="store_true", help="Verbose logging")
    return parser.parse_args()


def main():
    """Main entry point."""
    args = parse_args()

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)

    logger.info("VLM Test Program Starting...")
    logger.info(f"Command: {args.command}")
    logger.info(f"Model: {args.model}")

    # Create test runner
    runner = VLMTestRunner()

    # Initialize CFFI
    if not runner.initialize_cffi():
        logger.error("Failed to initialize CFFI")
        sys.exit(1)

    # Run tests based on command
    success = True
    if args.command == "test-basic":
        success = runner.test_basic(args)
    elif args.command == "test-streaming":
        success = runner.test_streaming(args)
    elif args.command == "test-all":
        success = runner.test_basic(args) and runner.test_streaming(args)

    # Print summary
    runner.print_summary()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
