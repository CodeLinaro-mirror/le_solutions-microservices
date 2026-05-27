# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM Image Cache Module

This module provides thread-safe caching for preprocessed VLM images.
Uses session/model cache keys to enable
efficient follow-up questions without re-downloading/preprocessing images.

Key features:
- Thread-safe operations with locks
- TTL-based expiration (configurable, default 30 minutes)
- LRU eviction when cache reaches max capacity
- Stores preprocessed image bytes (not raw images)
- Memory usage tracking and statistics
- Session/model key-based cache tracking
"""

import threading
import time
from typing import Optional, Dict
from openapi_server.logger.logger_config import LoggerConfig

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

IMAGE_CACHE_TTL = 1800  # 30 minutes
IMAGE_CACHE_MAX_ENTRIES = 100
IMAGE_CACHE_MAX_BYTES = 256 * 1024 * 1024  # 256 MiB


class ImageCache:
    """
    Thread-safe cache for preprocessed VLM images.

    Uses session/model keys to cache preprocessed image bytes across
    conversation turns. When users ask follow-up questions about an image
    without re-sending it, the cache provides the preprocessed bytes instantly.
    """

    def __init__(
        self,
        ttl_seconds: int = IMAGE_CACHE_TTL,
        max_entries: int = IMAGE_CACHE_MAX_ENTRIES,
        max_bytes: int = IMAGE_CACHE_MAX_BYTES,
    ):
        """
        Initialize the VLM image cache.

        Args:
            ttl_seconds: Time-to-live for cache entries in seconds (default: 30 minutes)
            max_entries: Maximum number of cached entries (default: 100)
            max_bytes: Maximum total cached image bytes (default: 256 MiB)
        """
        self._cache: Dict[str, dict] = {}
        self._lock = threading.Lock()
        self._ttl = ttl_seconds
        self._max_entries = max_entries
        self._max_bytes = max_bytes

        logger.info(
            f"VLM image cache initialized: TTL={ttl_seconds}s, "
            f"max_entries={max_entries}, max_bytes={max_bytes}"
        )

    def _is_entry_expired(self, entry: dict, current_time: float) -> bool:
        """Return True when an entry has been inactive longer than the TTL."""
        return current_time - entry['last_access'] > self._ttl

    def _current_size_locked(self) -> int:
        """Return total cached image bytes. Caller must hold ``self._lock``."""
        return sum(entry['size_bytes'] for entry in self._cache.values())

    def _cleanup_expired_locked(self, current_time: float) -> int:
        """Remove expired entries. Caller must hold ``self._lock``."""
        expired_keys = [
            key for key, entry in self._cache.items()
            if self._is_entry_expired(entry, current_time)
        ]
        for key in expired_keys:
            entry = self._cache[key]
            del self._cache[key]
            logger.debug(
                f"Removed expired cache entry: {key} "
                f"(inactive: {current_time - entry['last_access']:.1f}s)"
            )
        if expired_keys:
            logger.info(f"Cleanup removed {len(expired_keys)} expired cache entries")
        return len(expired_keys)

    def _evict_lru_locked(self, reason: str, current_time: float) -> bool:
        """Evict the least-recently-used entry. Caller must hold ``self._lock``."""
        if not self._cache:
            return False
        oldest_key = min(
            self._cache.keys(),
            key=lambda k: self._cache[k]['last_access']
        )
        evicted_entry = self._cache[oldest_key]
        del self._cache[oldest_key]
        logger.info(
            f"Evicted image cache entry for {reason}: {oldest_key} "
            f"(size: {evicted_entry['size_bytes']} bytes, "
            f"inactive: {current_time - evicted_entry['last_access']:.1f}s)"
        )
        return True

    def get(self, cache_key: str) -> Optional[bytes]:
        """
        Get cached preprocessed image bytes.

        Args:
            cache_key: Session/model cache key

        Returns:
            Preprocessed image bytes or None if not found/expired
        """
        if not cache_key:
            logger.debug("Empty cache key provided to cache.get()")
            return None

        with self._lock:
            if cache_key not in self._cache:
                logger.debug(f"Cache miss for key: {cache_key}")
                return None

            entry = self._cache[cache_key]

            current_time = time.time()

            # Check inactivity TTL expiration
            if self._is_entry_expired(entry, current_time):
                logger.info(
                    f"Cache entry expired for key: {cache_key} "
                    f"(inactive: {current_time - entry['last_access']:.1f}s)"
                )
                del self._cache[cache_key]
                return None

            # Update last access time for LRU tracking
            entry['last_access'] = current_time
            entry['access_count'] += 1

            logger.info(
                f"Cache hit for key: {cache_key} "
                f"(size: {len(entry['image_bytes'])} bytes, accesses: {entry['access_count']})"
            )
            return entry['image_bytes']

    def set(self, cache_key: str, image_bytes: bytes, image_source: str) -> None:
        """
        Cache preprocessed image bytes.

        Args:
            cache_key: Session/model cache key
            image_bytes: Preprocessed image bytes from image_preprocessor
            image_source: Source identifier for logging (URL snippet or "base64")
        """
        if not cache_key:
            logger.warning("Empty cache key provided to cache.set(), skipping cache")
            return

        if not image_bytes:
            logger.warning("Empty image bytes provided to cache.set(), skipping cache")
            return

        image_size = len(image_bytes)
        if self._max_bytes and image_size > self._max_bytes:
            logger.warning(
                f"Image for key {cache_key} is too large to cache: "
                f"{image_size} bytes > max {self._max_bytes} bytes"
            )
            return

        current_time = time.time()
        with self._lock:
            self._cleanup_expired_locked(current_time)

            # Remove the old value first so replacing the same key does not evict
            # unrelated entries or double-count memory.
            replaced_entry = self._cache.pop(cache_key, None)
            if replaced_entry:
                logger.debug(
                    f"Replacing image cache entry for {cache_key} "
                    f"(old size: {replaced_entry['size_bytes']} bytes)"
                )

            # Evict least-recently-used entries until both caps can be satisfied.
            while self._cache and len(self._cache) >= self._max_entries:
                self._evict_lru_locked("entry limit", current_time)

            while (
                self._cache and
                self._max_bytes and
                self._current_size_locked() + image_size > self._max_bytes
            ):
                self._evict_lru_locked("memory limit", current_time)

            # Store new entry
            self._cache[cache_key] = {
                'image_bytes': image_bytes,
                'image_source': image_source,
                'timestamp': current_time,
                'last_access': current_time,
                'access_count': 0,
                'size_bytes': image_size
            }

            total_size = self._current_size_locked()
            logger.info(
                f"Cached image for hash/key {cache_key}: "
                f"{image_size} bytes from {image_source} "
                f"(cache entries: {len(self._cache)}/{self._max_entries}, "
                f"cache bytes: {total_size}/{self._max_bytes})"
            )

    def clear(self, cache_key: str) -> bool:
        """
        Clear specific cache entry.

        Args:
            cache_key: Cache key to clear

        Returns:
            True if entry was found and cleared, False otherwise
        """
        with self._lock:
            if cache_key in self._cache:
                entry = self._cache[cache_key]
                del self._cache[cache_key]
                logger.info(
                    f"Cleared cache for key: {cache_key} "
                    f"(size: {entry['size_bytes']} bytes)"
                )
                return True
            else:
                logger.debug(f"Cache clear requested for non-existent key: {cache_key}")
                return False

    def clear_by_prefix(self, prefix: str) -> int:
        """
        Clear all cache entries whose key starts with the given prefix.

        Args:
            prefix: Key prefix to match (e.g., 'chat-abc123:')

        Returns:
            Number of entries cleared
        """
        if not prefix:
            return 0

        with self._lock:
            keys_to_remove = [k for k in self._cache if k.startswith(prefix)]
            for key in keys_to_remove:
                del self._cache[key]

            if keys_to_remove:
                logger.info(
                    f"Cleared {len(keys_to_remove)} cache entries with prefix '{prefix}' "
                    f"(cache size: {len(self._cache)}/{self._max_entries})"
                )
            return len(keys_to_remove)

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
        with self._lock:
            return self._cleanup_expired_locked(time.time())

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
                    'max_size_bytes': self._max_bytes,
                    'max_size_mb': self._max_bytes / (1024 * 1024),
                    'total_size_bytes': 0,
                    'total_size_mb': 0.0,
                    'ttl_seconds': self._ttl,
                    'oldest_entry_age_seconds': 0,
                    'oldest_entry_idle_seconds': 0,
                    'total_accesses': 0,
                    'average_entry_size_bytes': 0
                }

            current_time = time.time()
            total_size = sum(entry['size_bytes'] for entry in self._cache.values())
            total_accesses = sum(entry['access_count'] for entry in self._cache.values())
            oldest_age = max(current_time - entry['timestamp'] for entry in self._cache.values())
            oldest_idle = max(current_time - entry['last_access'] for entry in self._cache.values())

            return {
                'entries': len(self._cache),
                'max_entries': self._max_entries,
                'max_size_bytes': self._max_bytes,
                'max_size_mb': self._max_bytes / (1024 * 1024),
                'total_size_bytes': total_size,
                'total_size_mb': total_size / (1024 * 1024),
                'ttl_seconds': self._ttl,
                'oldest_entry_age_seconds': oldest_age,
                'oldest_entry_idle_seconds': oldest_idle,
                'total_accesses': total_accesses,
                'average_entry_size_bytes': total_size // len(self._cache) if self._cache else 0
            }

    def get_entry_info(self, cache_key: str) -> Optional[dict]:
        """
        Get information about a specific cache entry.

        Args:
            cache_key: Cache key to query

        Returns:
            Dictionary with entry info or None if not found
        """
        with self._lock:
            if cache_key not in self._cache:
                return None

            entry = self._cache[cache_key]
            current_time = time.time()

            return {
                'cache_key': cache_key,
                'size_bytes': entry['size_bytes'],
                'image_source': entry['image_source'],
                'age_seconds': current_time - entry['timestamp'],
                'time_since_last_access_seconds': current_time - entry['last_access'],
                'access_count': entry['access_count'],
                'expires_in_seconds': max(0, self._ttl - (current_time - entry['last_access']))
            }


# Global cache instance
_vlm_image_cache: Optional[ImageCache] = None
_cache_lock = threading.Lock()


def get_image_cache() -> ImageCache:
    """
    Get or create the global VLM image cache instance.

    This function ensures thread-safe singleton initialization of the cache.
    The cache is shared across all VLM requests to enable efficient image reuse.

    Returns:
        ImageCache: Global cache instance
    """
    global _vlm_image_cache

    if _vlm_image_cache is None:
        with _cache_lock:
            # Double-check locking pattern
            if _vlm_image_cache is None:
                _vlm_image_cache = ImageCache()
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
        _vlm_image_cache = ImageCache()
        logger.info("Reinitialized VLM image cache")


def get_cache_stats() -> dict:
    """
    Get statistics for the global VLM image cache.

    Returns:
        Dictionary with cache statistics, or empty dict if cache not initialized
    """
    cache = get_image_cache()
    return cache.get_stats()


def clear_session_images(session_id: str) -> int:
    """
    Clear all cached VLM images for a given session.

    Args:
        session_id: Session identifier (e.g., 'chat-abc123')

    Returns:
        Number of entries cleared
    """
    if not session_id:
        return 0

    cache = get_image_cache()
    count = cache.clear_by_prefix(f"{session_id}:")
    if count > 0:
        logger.info(f"Cleared {count} cached image(s) for session {session_id}")
    return count
