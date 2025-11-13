# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import List
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class TokenCounter:
    """
    Utility class to estimate token counts from text.
    Uses a simple word-based approximation (word count * 1.3).
    For production, consider integrating a proper tokenizer like tiktoken.
    """

    # Approximate ratio of tokens to words (conservative estimate)
    TOKENS_PER_WORD = 1.3

    @staticmethod
    def estimate_tokens(text) -> int:
        """
        Estimate the number of tokens in a text string or object.

        Args:
            text: Input text (can be string or object)

        Returns:
            Estimated token count
        """
        if not text:
            return 0

        if not isinstance(text, str):
            # Convert object to string representation
            text = str(text)

        # Simple word count estimation
        word_count = len(text.split())
        estimated_tokens = int(word_count * TokenCounter.TOKENS_PER_WORD)

        logger.debug(f"Estimated {estimated_tokens} tokens for {word_count} words")
        return estimated_tokens

    @staticmethod
    def estimate_tokens_for_messages(messages: List[dict]) -> int:
        """
        Estimate total token count for a list of messages.

        Args:
            messages: List of message dictionaries with 'role' and 'content'

        Returns:
            Total estimated token count
        """
        total_tokens = 0

        for message in messages:
            # Count tokens for role (approximately 4 tokens per message for formatting)
            total_tokens += 4

            # Count tokens for content
            content = message.get('content', '')
            if content:
                # Handle both string and object content
                content_str = str(content) if not isinstance(content, str) else content
                total_tokens += TokenCounter.estimate_tokens(content_str)

        logger.debug(f"Estimated {total_tokens} tokens for {len(messages)} messages")
        return total_tokens

    @staticmethod
    def estimate_tokens_with_overhead(text: str, overhead_tokens: int = 10) -> int:
        """
        Estimate tokens with additional overhead for formatting.

        Args:
            text: Input text
            overhead_tokens: Additional tokens for formatting/special tokens

        Returns:
            Estimated token count including overhead
        """
        base_tokens = TokenCounter.estimate_tokens(text)
        return base_tokens + overhead_tokens
