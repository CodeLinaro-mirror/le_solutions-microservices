# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Thread pool module for VLM operations.
This module is separate to avoid circular imports.
"""

import asyncio
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Optional, Any

# Global thread pool for VLM operations
vlm_thread_pool: Optional[ThreadPoolExecutor] = None

# Global lock for serializing VLM operations (ensures only one VLM request at a time)
vlm_operation_lock: Optional[asyncio.Lock] = None

# Pre-initialized CFFI objects (shared with VLMExecutionThread)
preinit_vlm_cffi: Optional[Any] = None  # FFI instance
preinit_vlm_lib: Optional[Any] = None   # Loaded library

# Pre-initialized VLM handle (optional, for handle reuse patterns)
preinit_vlm_handle: Optional[Any] = None


def get_vlm_thread_pool() -> Optional[ThreadPoolExecutor]:
    """
    Get the global VLM thread pool.

    Returns:
        ThreadPoolExecutor or None if not initialized
    """
    return vlm_thread_pool


def initialize_vlm_thread_pool(max_workers: int = 4) -> ThreadPoolExecutor:
    """
    Initialize the global VLM thread pool.

    Args:
        max_workers: Maximum number of worker threads

    Returns:
        The initialized ThreadPoolExecutor
    """
    global vlm_thread_pool
    vlm_thread_pool = ThreadPoolExecutor(max_workers=max_workers, thread_name_prefix="vlm_worker")
    return vlm_thread_pool


def shutdown_vlm_thread_pool(wait: bool = True) -> None:
    """
    Shutdown the global VLM thread pool.

    Args:
        wait: Whether to wait for threads to complete
    """
    global vlm_thread_pool
    if vlm_thread_pool:
        vlm_thread_pool.shutdown(wait=wait)
        vlm_thread_pool = None


def get_vlm_operation_lock() -> Optional[asyncio.Lock]:
    """
    Get the global VLM operation lock.

    This lock ensures that only one VLM operation runs at a time,
    which is critical for thread affinity requirements.

    Returns:
        asyncio.Lock or None if not initialized
    """
    return vlm_operation_lock


def initialize_vlm_operation_lock() -> asyncio.Lock:
    """
    Initialize the global VLM operation lock.

    Returns:
        The initialized asyncio.Lock
    """
    global vlm_operation_lock
    vlm_operation_lock = asyncio.Lock()
    return vlm_operation_lock


def get_preinit_vlm_cffi() -> Optional[Any]:
    """
    Get the pre-initialized CFFI FFI instance.

    Returns:
        FFI instance or None if not initialized
    """
    return preinit_vlm_cffi


def get_preinit_vlm_lib() -> Optional[Any]:
    """
    Get the pre-initialized CFFI library.

    Returns:
        Loaded library or None if not initialized
    """
    return preinit_vlm_lib


def set_preinit_vlm_cffi(ffi: Any, lib: Any) -> None:
    """
    Set the pre-initialized CFFI objects.

    Args:
        ffi: FFI instance
        lib: Loaded library
    """
    global preinit_vlm_cffi, preinit_vlm_lib
    preinit_vlm_cffi = ffi
    preinit_vlm_lib = lib


def get_preinit_vlm_handle() -> Optional[Any]:
    """
    Get the pre-initialized VLM handle.

    Returns:
        VLM handle or None if not initialized
    """
    return preinit_vlm_handle


def set_preinit_vlm_handle(handle: Any) -> None:
    """
    Set the pre-initialized VLM handle.

    Args:
        handle: VLM handle
    """
    global preinit_vlm_handle
    preinit_vlm_handle = handle


def clear_preinit_vlm_objects() -> None:
    """
    Clear all pre-initialized VLM objects.

    This should be called during shutdown to ensure proper cleanup.
    """
    global preinit_vlm_cffi, preinit_vlm_lib, preinit_vlm_handle
    preinit_vlm_cffi = None
    preinit_vlm_lib = None
    preinit_vlm_handle = None
