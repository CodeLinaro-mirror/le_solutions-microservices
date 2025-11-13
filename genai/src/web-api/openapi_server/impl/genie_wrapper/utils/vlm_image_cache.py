# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM Image Cache Module

This module provides thread-safe caching for preprocessed VLM images.
Uses conversation hash from chat_utils as session identifier to enable
efficient follow-up questions without re-downloading/preprocessing images.

Key features:
- Thread-safe operations with locks
- TTL-based expiration (configurable, default 30 minutes)
- LRU eviction when cache reaches max capacity
- Stores preprocessed image bytes (not raw images)
- Memory usage tracking and statistics
- Conversation hash-based session tracking
"""

import threading
import time
from typing import Optional, Dict, Tuple
from openapi_server.logger.logger_config import LoggerConfig

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class VLMImageCache:
    """
    Thread-safe cache for preprocessed VLM images.

    Uses conversation hash from chat_utils as session identifier to enable
    efficient caching across conversation turns. When users ask follow-up
    questions about an image without re-sending it, the cache provides the
    preprocessed image bytes instantly.
    """

    def __init__(self, ttl_seconds: int = 1800, max_entries: int = 100):
        """
        Initialize the VLM image cache.

        Args:
            ttl_seconds: Time-to-live for cache entries in seconds (default: 30 minutes)
            max_entries: Maximum number of cached entries (default: 100)
        """
        self._cache: Dict[str, dict] = {}
        self._lock = threading.Lock()
        self._ttl = ttl_seconds
        self._max_entries = max_entries

        logger.info(f"VLM image cache initialized: TTL={ttl_seconds}s, max_entries={max_entries}")

    def get(self, conversation_hash: str) -> Optional[bytes]:
        """
        Get cached preprocessed image bytes for a conversation.

        Args:
            conversation_hash: Conversation hash from chat_utils.calculate_conversation_hash()

        Returns:
            Preprocessed image bytes or None if not found/expired
        """
        if not conversation_hash:
            logger.debug("Empty conversation hash provided to cache.get()")
            return None

        with self._lock:
            if conversation_hash not in self._cache:
                logger.debug(f"Cache miss for conversation hash: {conversation_hash}")
                return None

            entry = self._cache[conversation_hash]

            # Check TTL expiration
            if time.time() - entry['timestamp'] > self._ttl:
                logger.info(f"Cache entry expired for hash: {conversation_hash} (age: {time.time() - entry['timestamp']:.1f}s)")
                del self._cache[conversation_hash]
                return None

            # Update last access time for LRU tracking
            entry['last_access'] = time.time()
            entry['access_count'] += 1

            logger.info(f"Cache hit for hash: {conversation_hash} (size: {len(entry['image_bytes'])} bytes, accesses: {entry['access_count']})")
            return entry['image_bytes']

    def set(self, conversation_hash: str, image_bytes: bytes, image_source: str) -> None:
        """
        Cache preprocessed image bytes for a conversation.

        Args:
            conversation_hash: Conversation hash from chat_utils.calculate_conversation_hash()
            image_bytes: Preprocessed image bytes from image_preprocessor
            image_source: Source identifier for logging (URL snippet or "base64")
        """
        if not conversation_hash:
            logger.warning("Empty conversation hash provided to cache.set(), skipping cache")
            return

        if not image_bytes:
            logger.warning("Empty image bytes provided to cache.set(), skipping cache")
            return

        with self._lock:
            # Evict oldest entry if at max capacity
            if len(self._cache) >= self._max_entries:
                oldest_key = min(
                    self._cache.keys(),
                    key=lambda k: self._cache[k]['last_access']
                )
                evicted_entry = self._cache[oldest_key]
                logger.info(
                    f"Cache full ({len(self._cache)}/{self._max_entries}), "
                    f"evicting oldest entry: {oldest_key} "
                    f"(size: {len(evicted_entry['image_bytes'])} bytes, "
                    f"age: {time.time() - evicted_entry['timestamp']:.1f}s)"
                )
                del self._cache[oldest_key]

            # Store new entry
            current_time = time.time()
            self._cache[conversation_hash] = {
                'image_bytes': image_bytes,
                'image_source': image_source,
                'timestamp': current_time,
                'last_access': current_time,
                'access_count': 0,
                'size_bytes': len(image_bytes)
            }

            logger.info(
                f"Cached image for hash {conversation_hash}: "
                f"{len(image_bytes)} bytes from {image_source} "
                f"(cache size: {len(self._cache)}/{self._max_entries})"
            )

    def clear(self, conversation_hash: str) -> bool:
        """
        Clear specific cache entry.

        Args:
            conversation_hash: Conversation hash to clear

        Returns:
            True if entry was found and cleared, False otherwise
        """
        with self._lock:
            if conversation_hash in self._cache:
                entry = self._cache[conversation_hash]
                del self._cache[conversation_hash]
                logger.info(
                    f"Cleared cache for hash: {conversation_hash} "
                    f"(size: {entry['size_bytes']} bytes)"
                )
                return True
            else:
                logger.debug(f"Cache clear requested for non-existent hash: {conversation_hash}")
                return False

    def clear_all(self) -> int:
        """
        Clear all cache entries.

        Returns:
            Number of entries cleared
        """
        with self._lock:
            count = len(self._cache)
            total_size = sum(entry['size_bytes'] for entry in self._cache.values())
            self._cache.clear()
            logger.info(f"Cleared all cache entries: {count} entries, {total_size} bytes")
            return count

    def cleanup_expired(self) -> int:
        """
        Remove expired entries from cache.

        Returns:
            Number of entries removed
        """
        current_time = time.time()
        expired_keys = []

        with self._lock:
            for key, entry in self._cache.items():
                if current_time - entry['timestamp'] > self._ttl:
                    expired_keys.append(key)

            for key in expired_keys:
                entry = self._cache[key]
                del self._cache[key]
                logger.debug(
                    f"Removed expired cache entry: {key} "
                    f"(age: {current_time - entry['timestamp']:.1f}s)"
                )

        if expired_keys:
            logger.info(f"Cleanup removed {len(expired_keys)} expired cache entries")

        return len(expired_keys)

    def get_stats(self) -> dict:
        """
        Get cache statistics.

        Returns:
            Dictionary with cache statistics
        """
        with self._lock:
            if not self._cache:
                return {
                    'entries': 0,
                    'max_entries': self._max_entries,
                    'total_size_bytes': 0,
                    'total_size_mb': 0.0,
                    'ttl_seconds': self._ttl,
                    'oldest_entry_age_seconds': 0,
                    'total_accesses': 0,
                    'average_entry_size_bytes': 0
                }

            current_time = time.time()
            total_size = sum(entry['size_bytes'] for entry in self._cache.values())
            total_accesses = sum(entry['access_count'] for entry in self._cache.values())
            oldest_age = max(current_time - entry['timestamp'] for entry in self._cache.values())

            return {
                'entries': len(self._cache),
                'max_entries': self._max_entries,
                'total_size_bytes': total_size,
                'total_size_mb': total_size / (1024 * 1024),
                'ttl_seconds': self._ttl,
                'oldest_entry_age_seconds': oldest_age,
                'total_accesses': total_accesses,
                'average_entry_size_bytes': total_size // len(self._cache) if self._cache else 0
            }

    def get_entry_info(self, conversation_hash: str) -> Optional[dict]:
        """
        Get information about a specific cache entry.

        Args:
            conversation_hash: Conversation hash to query

        Returns:
            Dictionary with entry info or None if not found
        """
        with self._lock:
            if conversation_hash not in self._cache:
                return None

            entry = self._cache[conversation_hash]
            current_time = time.time()

            return {
                'conversation_hash': conversation_hash,
                'size_bytes': entry['size_bytes'],
                'image_source': entry['image_source'],
                'age_seconds': current_time - entry['timestamp'],
                'time_since_last_access_seconds': current_time - entry['last_access'],
                'access_count': entry['access_count'],
                'expires_in_seconds': self._ttl - (current_time - entry['timestamp'])
            }


# Global cache instance
_vlm_image_cache: Optional[VLMImageCache] = None
_cache_lock = threading.Lock()


def get_vlm_image_cache() -> VLMImageCache:
    """
    Get or create the global VLM image cache instance.

    This function ensures thread-safe singleton initialization of the cache.
    The cache is shared across all VLM requests to enable efficient image reuse.

    Returns:
        VLMImageCache: Global cache instance
    """
    global _vlm_image_cache

    if _vlm_image_cache is None:
        with _cache_lock:
            # Double-check locking pattern
            if _vlm_image_cache is None:
                _vlm_image_cache = VLMImageCache()
                logger.info("Initialized global VLM image cache")

    return _vlm_image_cache


def reset_vlm_image_cache() -> None:
    """
    Reset the global VLM image cache (primarily for testing).

    This function clears all cached entries and reinitializes the cache.
    Should be used carefully in production environments.
    """
    global _vlm_image_cache

    with _cache_lock:
        if _vlm_image_cache is not None:
            count = _vlm_image_cache.clear_all()
            logger.info(f"Reset VLM image cache, cleared {count} entries")
        _vlm_image_cache = VLMImageCache()
        logger.info("Reinitialized VLM image cache")


def get_cache_stats() -> dict:
    """
    Get statistics for the global VLM image cache.

    Returns:
        Dictionary with cache statistics, or empty dict if cache not initialized
    """
    cache = get_vlm_image_cache()
    return cache.get_stats()
