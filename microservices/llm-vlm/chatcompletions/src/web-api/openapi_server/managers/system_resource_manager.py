# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""
System Resource Manager - Singleton for tracking system memory and managing process resources.
Implements LRU eviction policy for idle processes when memory is insufficient.
"""

import psutil
import threading
import time
from typing import Dict, Optional, Tuple, List
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ProcessInfo:
    """Information about a registered process."""

    def __init__(self, process_id: str, model_name: str, memory_mb: int, is_active: bool = False):
        self.process_id = process_id
        self.model_name = model_name
        self.memory_mb = int(memory_mb)
        self.is_active = is_active
        self.last_accessed = time.time()
        self.created_at = time.time()

    def mark_active(self):
        """Mark process as actively executing."""
        self.is_active = True
        self.last_accessed = time.time()

    def mark_idle(self):
        """Mark process as idle (not executing)."""
        self.is_active = False
        self.last_accessed = time.time()

    def update_access_time(self):
        """Update last accessed timestamp."""
        self.last_accessed = time.time()


class SystemResourceManager:
    """
    Singleton manager for system resources.
    Tracks memory usage and manages process lifecycle with LRU eviction.
    """

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super(SystemResourceManager, cls).__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if self._initialized:
            return

        self._initialized = True
        self._process_registry: Dict[str, ProcessInfo] = {}
        self._registry_lock = threading.Lock()

        # Configuration (defaults from SystemResourceConstants)
        from openapi_server.impl.constant import SystemResourceConstants
        self._memory_headroom_percent = SystemResourceConstants.MEMORY_HEADROOM_PERCENT
        self._guardrails_enabled = SystemResourceConstants.ENABLE_RESOURCE_GUARDRAILS

        # Max active models configuration
        import os
        try:
            max_active = os.getenv("MAX_ACTIVE_MODELS", "3")
            self._max_active_models = int(max_active)
        except ValueError:
            logger.warning(f"Invalid MAX_ACTIVE_MODELS value: {max_active}, defaulting to 3")
            self._max_active_models = 3

        logger.info(f"SystemResourceManager initialized (max_active_models={self._max_active_models})")

    def configure(self, memory_headroom_percent: int = 15, enable_guardrails: bool = True):
        """
        Configure the resource manager.

        Args:
            memory_headroom_percent: Percentage of memory to keep as headroom (default 15%)
            enable_guardrails: Whether to enable resource guardrails (default True)
        """
        self._memory_headroom_percent = memory_headroom_percent
        self._guardrails_enabled = enable_guardrails
        logger.info(f"SystemResourceManager configured: headroom={memory_headroom_percent}%, "
                   f"guardrails_enabled={enable_guardrails}")

    def get_available_memory_mb(self) -> int:
        """
        Get available system memory in MB.

        Returns:
            Available memory in megabytes
        """
        try:
            mem = psutil.virtual_memory()
            available_mb = mem.available // (1024 * 1024)
            return available_mb
        except Exception as e:
            logger.error(f"Error getting available memory: {e}")
            return 0

    def get_total_memory_mb(self) -> int:
        """
        Get total system memory in MB.

        Returns:
            Total memory in megabytes
        """
        try:
            mem = psutil.virtual_memory()
            total_mb = mem.total // (1024 * 1024)
            return total_mb
        except Exception as e:
            logger.error(f"Error getting total memory: {e}")
            return 0

    def calculate_required_memory_with_headroom(self, model_memory_mb: int) -> int:
        """
        Calculate required memory including headroom.

        Args:
            model_memory_mb: Memory required by the model

        Returns:
            Total memory required including headroom
        """
        # Ensure model_memory_mb is an integer
        memory_mb = int(model_memory_mb)
        headroom = int(memory_mb * (self._memory_headroom_percent / 100.0))
        return memory_mb + headroom

    def register_process(self, process_id: str, model_name: str, memory_mb: int, is_active: bool = False):
        """
        Register a new process in the resource manager.

        Args:
            process_id: Unique identifier for the process
            model_name: Name of the model being run
            memory_mb: Memory requirement in MB
            is_active: Whether the process is actively executing
        """
        with self._registry_lock:
            process_info = ProcessInfo(process_id, model_name, int(memory_mb), is_active)
            self._process_registry[process_id] = process_info
            logger.info(f"Registered process {process_id} for model {model_name} "
                       f"({memory_mb}MB, active={is_active})")

    def unregister_process(self, process_id: str):
        """
        Unregister a process from the resource manager.

        Args:
            process_id: Unique identifier for the process
        """
        with self._registry_lock:
            if process_id in self._process_registry:
                process_info = self._process_registry.pop(process_id)
                logger.info(f"Unregistered process {process_id} for model {process_info.model_name}")
            else:
                logger.warning(f"Attempted to unregister unknown process {process_id}")

    def mark_process_active(self, process_id: str):
        """
        Mark a process as actively executing.

        Args:
            process_id: Unique identifier for the process
        """
        with self._registry_lock:
            if process_id in self._process_registry:
                self._process_registry[process_id].mark_active()
                logger.debug(f"Marked process {process_id} as active")

    def mark_process_idle(self, process_id: str):
        """
        Mark a process as idle (not executing).

        Args:
            process_id: Unique identifier for the process
        """
        with self._registry_lock:
            if process_id in self._process_registry:
                self._process_registry[process_id].mark_idle()
                logger.debug(f"Marked process {process_id} as idle")

    def update_process_access_time(self, process_id: str):
        """
        Update the last accessed time for a process.

        Args:
            process_id: Unique identifier for the process
        """
        with self._registry_lock:
            if process_id in self._process_registry:
                self._process_registry[process_id].update_access_time()

    def get_idle_processes_lru(self) -> List[Tuple[str, ProcessInfo]]:
        """
        Get list of idle processes sorted by LRU (least recently used first).

        Returns:
            List of (process_id, ProcessInfo) tuples sorted by last_accessed time
        """
        with self._registry_lock:
            idle_processes = [
                (pid, info) for pid, info in self._process_registry.items()
                if not info.is_active
            ]
            # Sort by last_accessed (oldest first)
            idle_processes.sort(key=lambda x: x[1].last_accessed)
            return idle_processes

    def get_total_registered_memory_mb(self) -> int:
        """
        Get total memory used by all registered processes.

        Returns:
            Total memory in MB
        """
        with self._registry_lock:
            return sum(info.memory_mb for info in self._process_registry.values())

    def check_memory_availability(self, required_memory_mb: int) -> Tuple[bool, int, List[str]]:
        """
        Check if sufficient memory is available for a new process.
        If not, determine which idle processes should be evicted (LRU order).

        Args:
            required_memory_mb: Memory required by the new process (without headroom)

        Returns:
            Tuple of (sufficient_memory, available_mb, processes_to_evict)
            - sufficient_memory: True if memory is available (possibly after evictions)
            - available_mb: Current available memory
            - processes_to_evict: List of process IDs to evict (empty if sufficient memory)
        """
        if not self._guardrails_enabled:
            # Guardrails disabled, always allow
            return True, self.get_available_memory_mb(), []

        # Calculate required memory with headroom
        required_with_headroom = self.calculate_required_memory_with_headroom(required_memory_mb)

        # Get current available memory
        available_mb = self.get_available_memory_mb()

        # Check MAX_ACTIVE_MODELS constraint first
        active_count, idle_count = self.get_process_count()
        total_processes = active_count + idle_count

        processes_to_evict = []
        freed_memory = 0
        idle_processes = self.get_idle_processes_lru()

        # If we hit the max active models limit, we MUST evict idle processes
        if total_processes >= self._max_active_models:
            logger.info(f"Max active models limit reached ({total_processes} >= {self._max_active_models}). Evicting idle processes...")

            # Calculate how many we need to evict to make room for 1 new process
            # We need (total_processes - max_active_models + 1) evictions
            # e.g. if max=3, total=3, we need to evict 1 so total becomes 2, then +1 new = 3.
            needed_evictions = total_processes - self._max_active_models + 1

            if len(idle_processes) < needed_evictions:
                logger.warning(f"Cannot start new process: Max active models limit reached ({self._max_active_models}) and not enough idle processes to evict.")
                return False, available_mb, []

            # Mark for eviction to satisfy count constraint
            for i in range(needed_evictions):
                process_id, process_info = idle_processes[i]
                processes_to_evict.append(process_id)
                freed_memory += process_info.memory_mb
                logger.info(f"Evicting process {process_id} to satisfy MAX_ACTIVE_MODELS limit")

            # Remove evicted from idle list so we don't double count if we need more for memory
            idle_processes = idle_processes[needed_evictions:]

        logger.info(f"Memory check: required={required_memory_mb}MB, "
                   f"with_headroom={required_with_headroom}MB, available={available_mb}MB")

        # Check if we already have sufficient memory (including freed from count eviction)
        if available_mb + freed_memory >= required_with_headroom:
            logger.info("Sufficient memory available")
            return True, available_mb, processes_to_evict

        logger.info(f"Insufficient memory. Need to free {required_with_headroom - (available_mb + freed_memory)}MB. "
                   f"Found {len(idle_processes)} remaining idle processes")

        # Evict idle processes in LRU order until we have enough memory
        for process_id, process_info in idle_processes:
            processes_to_evict.append(process_id)
            freed_memory += process_info.memory_mb

            logger.info(f"Planning to evict process {process_id} ({process_info.model_name}, "
                       f"{process_info.memory_mb}MB, idle for {time.time() - process_info.last_accessed:.1f}s)")

            # Check if we now have enough memory
            if available_mb + freed_memory >= required_with_headroom:
                logger.info(f"After evicting {len(processes_to_evict)} processes, "
                           f"will have {available_mb + freed_memory}MB available")
                return True, available_mb, processes_to_evict

        # Even after evicting all idle processes, still insufficient
        logger.warning(f"Insufficient memory even after evicting all {len(idle_processes)} idle processes. "
                      f"Would have {available_mb + freed_memory}MB, need {required_with_headroom}MB")
        return False, available_mb, processes_to_evict

    def get_process_count(self) -> Tuple[int, int]:
        """
        Get count of active and idle processes.

        Returns:
            Tuple of (active_count, idle_count)
        """
        with self._registry_lock:
            active = sum(1 for info in self._process_registry.values() if info.is_active)
            idle = len(self._process_registry) - active
            return active, idle

    def get_memory_stats(self) -> Dict:
        """
        Get comprehensive memory statistics.

        Returns:
            Dictionary with memory statistics
        """
        available_mb = self.get_available_memory_mb()
        total_mb = self.get_total_memory_mb()
        registered_mb = self.get_total_registered_memory_mb()
        active_count, idle_count = self.get_process_count()

        return {
            "total_memory_mb": total_mb,
            "available_memory_mb": available_mb,
            "used_memory_mb": total_mb - available_mb,
            "registered_processes_memory_mb": registered_mb,
            "active_processes": active_count,
            "idle_processes": idle_count,
            "total_processes": active_count + idle_count,
            "memory_headroom_percent": self._memory_headroom_percent,
            "guardrails_enabled": self._guardrails_enabled
        }
