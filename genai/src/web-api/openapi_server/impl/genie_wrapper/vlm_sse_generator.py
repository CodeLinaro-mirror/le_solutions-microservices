# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM SSE Generator

This module provides Server-Sent Events (SSE) simulation for VLM responses.
Takes a complete VLM response from non-streaming execution and chunks it
for progressive streaming delivery to maintain OpenAI API compatibility.

Key features:
- Simulates streaming from complete responses
- OpenAI-compatible SSE format
- Configurable chunk sizes
- Proper timing simulation
- Error handling and cleanup
"""

import asyncio
import json
import uuid
import time
from typing import Dict, Any, AsyncGenerator, Optional
from openapi_server.logger.logger_config import LoggerConfig

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class VLMSSEGenerator:
    """
    Simulates Server-Sent Events (SSE) streaming for VLM responses.
    Takes a complete VLM response and chunks it for streaming delivery.
    """

    def __init__(self, response_data: Dict[str, Any], chunk_size: int = 5, delay_ms: int = 50):
        """
        Initialize the SSE generator.

        Args:
            response_data: Complete VLM response from non-streaming execution
            chunk_size: Number of words per chunk (default: 5)
            delay_ms: Delay between chunks in milliseconds (default: 50ms)
        """
        self.response_data = response_data
        self.chunk_size = chunk_size
        self.delay_seconds = delay_ms / 1000.0
        self.completion_id = response_data.get("id", f"chatcmpl-{uuid.uuid4().hex[:24]}")
        self.model = response_data.get("model", "")
        self.created = response_data.get("created", int(time.time()))
        self.role = response_data.get("role", "assistant")
        self.finish_reason = response_data.get("finish_reason", "stop")

        # Extract and prepare content for chunking
        self.content = response_data.get("content", "")
        self.words = self._split_into_words(self.content)
        self.chunks = self._create_chunks()

        logger.info(f"VLMSSEGenerator initialized: {len(self.words)} words, {len(self.chunks)} chunks")

    def _split_into_words(self, content: str) -> list:
        """
        Split content into words while preserving whitespace and punctuation.

        Args:
            content: Text content to split

        Returns:
            List of word tokens with preserved spacing
        """
        if not content:
            return []

        # Simple word splitting that preserves spaces
        words = []
        current_word = ""

        for char in content:
            if char.isspace():
                if current_word:
                    words.append(current_word)
                    current_word = ""
                words.append(char)  # Preserve the whitespace
            else:
                current_word += char

        # Add the last word if any
        if current_word:
            words.append(current_word)

        return words

    def _create_chunks(self) -> list:
        """
        Create chunks from words based on chunk_size.

        Returns:
            List of text chunks
        """
        if not self.words:
            return [""]

        chunks = []
        current_chunk = ""
        word_count = 0

        for word in self.words:
            current_chunk += word

            # Count non-whitespace words
            if not word.isspace():
                word_count += 1

            # Create chunk when we reach chunk_size words
            if word_count >= self.chunk_size:
                chunks.append(current_chunk)
                current_chunk = ""
                word_count = 0

        # Add remaining content as final chunk
        if current_chunk:
            chunks.append(current_chunk)

        # Ensure we have at least one chunk
        if not chunks:
            chunks = [""]

        return chunks

    async def generate_sse_chunks(self) -> AsyncGenerator[str, None]:
        """
        Generate SSE-formatted chunks from the complete response.
        Yields OpenAI-compatible SSE chunks with proper timing.

        Yields:
            str: SSE formatted response chunks
        """
        try:
            logger.info(f"Starting SSE generation for completion {self.completion_id}")

            # Send initial chunk with role
            first_chunk = {
                "id": self.completion_id,
                "object": "chat.completion.chunk",
                "created": self.created,
                "model": self.model,
                "choices": [{
                    "index": 0,
                    "delta": {
                        "role": self.role
                    },
                    "finish_reason": None
                }]
            }

            yield f"data: {json.dumps(first_chunk)}\n\n"
            await asyncio.sleep(self.delay_seconds)

            # Send content chunks
            for i, chunk_content in enumerate(self.chunks):
                if chunk_content:  # Only send non-empty chunks
                    chunk = {
                        "id": self.completion_id,
                        "object": "chat.completion.chunk",
                        "created": self.created,
                        "model": self.model,
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "content": chunk_content
                            },
                            "finish_reason": None
                        }]
                    }

                    yield f"data: {json.dumps(chunk)}\n\n"

                    # Add delay between chunks (except for the last one)
                    if i < len(self.chunks) - 1:
                        await asyncio.sleep(self.delay_seconds)

            # Send final chunk with finish_reason
            final_chunk = {
                "id": self.completion_id,
                "object": "chat.completion.chunk",
                "created": self.created,
                "model": self.model,
                "choices": [{
                    "index": 0,
                    "delta": {},
                    "finish_reason": self.finish_reason
                }]
            }

            yield f"data: {json.dumps(final_chunk)}\n\n"

            # Send [DONE] marker
            yield "data: [DONE]\n\n"

            logger.info(f"SSE generation completed for completion {self.completion_id}")

        except Exception as e:
            logger.error(f"Error in SSE generation: {e}", exc_info=True)

            # Send error chunk
            error_chunk = {
                "id": self.completion_id,
                "object": "chat.completion.chunk",
                "created": self.created,
                "model": self.model,
                "choices": [{
                    "index": 0,
                    "delta": {},
                    "finish_reason": "error"
                }]
            }

            yield f"data: {json.dumps(error_chunk)}\n\n"
            yield "data: [DONE]\n\n"

    @staticmethod
    def create_error_sse(error_message: str, completion_id: Optional[str] = None) -> AsyncGenerator[str, None]:
        """
        Create an SSE stream for error responses.

        Args:
            error_message: Error message to include
            completion_id: Optional completion ID

        Yields:
            str: SSE formatted error response
        """
        async def error_generator():
            try:
                if not completion_id:
                    error_id = f"chatcmpl-{uuid.uuid4().hex[:24]}"
                else:
                    error_id = completion_id

                error_chunk = {
                    "id": error_id,
                    "object": "chat.completion.chunk",
                    "created": int(time.time()),
                    "model": "",
                    "choices": [{
                        "index": 0,
                        "delta": {
                            "content": f"Error: {error_message}"
                        },
                        "finish_reason": "error"
                    }]
                }

                yield f"data: {json.dumps(error_chunk)}\n\n"
                yield "data: [DONE]\n\n"

            except Exception as e:
                logger.error(f"Error in error SSE generation: {e}")
                yield "data: [ERROR]\n\n"

        return error_generator()


class VLMSSEConfig:
    """Configuration class for VLM SSE generation."""

    # Default chunk sizes for different content lengths
    SMALL_CONTENT_CHUNK_SIZE = 3   # For content < 50 words
    MEDIUM_CONTENT_CHUNK_SIZE = 5  # For content 50-200 words
    LARGE_CONTENT_CHUNK_SIZE = 8   # For content > 200 words

    # Default delays (in milliseconds)
    FAST_DELAY = 30    # Fast streaming
    NORMAL_DELAY = 50  # Normal streaming
    SLOW_DELAY = 100   # Slow streaming

    @staticmethod
    def get_optimal_config(content: str) -> tuple:
        """
        Get optimal chunk size and delay based on content length.

        Args:
            content: Text content to analyze

        Returns:
            tuple: (chunk_size, delay_ms)
        """
        word_count = len(content.split()) if content else 0

        if word_count < 50:
            return VLMSSEConfig.SMALL_CONTENT_CHUNK_SIZE, VLMSSEConfig.FAST_DELAY
        elif word_count < 200:
            return VLMSSEConfig.MEDIUM_CONTENT_CHUNK_SIZE, VLMSSEConfig.NORMAL_DELAY
        else:
            return VLMSSEConfig.LARGE_CONTENT_CHUNK_SIZE, VLMSSEConfig.SLOW_DELAY


def create_vlm_sse_generator(response_data: Dict[str, Any],
                           chunk_size: Optional[int] = None,
                           delay_ms: Optional[int] = None) -> VLMSSEGenerator:
    """
    Factory function to create a VLM SSE generator with optimal settings.

    Args:
        response_data: Complete VLM response data
        chunk_size: Optional custom chunk size
        delay_ms: Optional custom delay in milliseconds

    Returns:
        VLMSSEGenerator: Configured SSE generator
    """
    content = response_data.get("content", "")

    # Use provided values or get optimal configuration
    if chunk_size is None or delay_ms is None:
        optimal_chunk_size, optimal_delay = VLMSSEConfig.get_optimal_config(content)
        chunk_size = chunk_size or optimal_chunk_size
        delay_ms = delay_ms or optimal_delay

    logger.info(f"Creating VLM SSE generator: chunk_size={chunk_size}, delay_ms={delay_ms}")

    return VLMSSEGenerator(response_data, chunk_size, delay_ms)
