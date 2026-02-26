# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import asyncio
from typing import Dict, Optional
from common.logger import get_logger

logger = get_logger(__name__)


class ServiceCoordinator:
    """
    Coordinates cleanup between services (ASR, T2T, TTS) to prevent resource conflicts.
    Ensures one service is fully cleaned up before the other starts processing.
    """
    
    def __init__(self):
        self._cleanup_locks: Dict[str, asyncio.Lock] = {
            'ASR': asyncio.Lock(),
            'T2T': asyncio.Lock(),
            'TTS': asyncio.Lock()
        }
        self._active_service: Optional[str] = None
        self._coordinator_lock = asyncio.Lock()
        self._cleanup_callbacks: Dict[str, callable] = {}
        self._tts_keep_alive: bool = True  # Track TTS keep_alive state
        self._asr_keep_alive: bool = True  # Track ASR keep_alive state
        
    async def request_service_start(self, service_name: str, cleanup_callback) -> bool:
        """
        Request to start a service. If another service is active, it will be cleaned up first.
        
        Args:
            service_name: Name of the service requesting to start ('ASR', 'T2T', or 'TTS')
            cleanup_callback: Async function to call for cleanup of this service
            
        Returns:
            True if service can proceed, False otherwise
        """
        # Store the cleanup callback for this service
        self._cleanup_callbacks[service_name] = cleanup_callback
        
        # Ensure the service has a cleanup lock (dynamic registration)
        if service_name not in self._cleanup_locks:
            self._cleanup_locks[service_name] = asyncio.Lock()
            logger.info(f"Registered new service: {service_name}")
        
        async with self._coordinator_lock:
            # If this service is already active, no need to do anything
            if self._active_service == service_name:
                logger.debug(f"{service_name} is already active, proceeding")
                return True
            
            # If another service is active, clean it up first
            if self._active_service is not None and self._active_service != service_name:
                other_service = self._active_service
                
                # Keep-alive: skip cleanup and switch active service immediately, no sleep needed
                if other_service == 'TTS' and self._tts_keep_alive:
                    logger.info(f"{service_name} requested, TTS keep_alive enabled — skipping TTS cleanup")
                    self._active_service = service_name
                    logger.info(f"{service_name} is now the active service (TTS kept alive)")
                    return True
                
                if other_service == 'ASR' and self._asr_keep_alive:
                    logger.info(f"{service_name} requested, ASR keep_alive enabled — skipping ASR cleanup")
                    self._active_service = service_name
                    logger.info(f"{service_name} is now the active service (ASR kept alive)")
                    return True
                
                # Background cleanup — switch active service immediately so the
                # requesting service is not blocked, then clean up the old one
                # in the background. The 0.25s DSP settle sleep stays in the
                # background task so it never delays the caller.
                logger.info(f"{service_name} requested, scheduling background cleanup of {other_service}")
                other_cleanup = self._cleanup_callbacks.get(other_service)
                if other_cleanup:
                    if other_service not in self._cleanup_locks:
                        self._cleanup_locks[other_service] = asyncio.Lock()

                    async def _run_cleanup(svc=other_service, fn=other_cleanup):
                        async with self._cleanup_locks[svc]:
                            try:
                                logger.info(f"Background cleanup: calling {svc} cleanup...")
                                await fn()
                                logger.info(f"Background cleanup: {svc} cleanup completed")
                                await asyncio.sleep(0.25)
                                logger.info(f"Background cleanup: {svc} resources fully released")
                            except Exception as e:
                                logger.error(f"Background cleanup error for {svc}: {e}", exc_info=True)

                    asyncio.create_task(_run_cleanup())
                else:
                    logger.warning(f"No cleanup callback registered for {other_service}")
            
            # Mark this service as active
            self._active_service = service_name
            logger.info(f"{service_name} is now the active service")
            return True
    
    async def cleanup_if_not_active(self, service_name: str, cleanup_callback) -> bool:
        """
        Check if this service should cleanup because another service is now active.
        
        Args:
            service_name: Name of the service checking ('ASR', 'T2T', or 'TTS')
            cleanup_callback: Async function to call for cleanup
            
        Returns:
            True if cleanup was performed, False if service is still active
        """
        async with self._coordinator_lock:
            # If this service is not the active one, perform cleanup
            if self._active_service != service_name:
                logger.info(f"{service_name} detected it's not active (active: {self._active_service}), performing cleanup")
                
                # Ensure the service has a cleanup lock
                if service_name not in self._cleanup_locks:
                    self._cleanup_locks[service_name] = asyncio.Lock()
                
                # Acquire this service's cleanup lock while cleaning up
                async with self._cleanup_locks[service_name]:
                    try:
                        await cleanup_callback()
                        logger.info(f"{service_name} cleanup completed")
                        return True
                    except Exception as e:
                        logger.error(f"Error during {service_name} cleanup: {e}", exc_info=True)
                        return True
            
            return False
    
    def get_active_service(self) -> Optional[str]:
        """Get the currently active service name."""
        return self._active_service
    
    async def release_service(self, service_name: str):
        """
        Release a service (mark it as no longer active).
        
        Args:
            service_name: Name of the service to release
        """
        async with self._coordinator_lock:
            if self._active_service == service_name:
                logger.info(f"{service_name} released")
                self._active_service = None
    
    def set_tts_keep_alive(self, keep_alive: bool):
        """
        Set the TTS keep_alive state.
        
        Args:
            keep_alive: True to keep TTS alive when other services start, False to cleanup normally
        """
        self._tts_keep_alive = keep_alive
        logger.info(f"TTS keep_alive set to: {keep_alive}")
    
    def get_tts_keep_alive(self) -> bool:
        """
        Get the current TTS keep_alive state.
        
        Returns:
            True if TTS should be kept alive, False otherwise
        """
        return self._tts_keep_alive

    def set_asr_keep_alive(self, keep_alive: bool):
        """
        Set the ASR keep_alive state.
        
        Args:
            keep_alive: True to keep ASR alive when other services start, False to cleanup normally
        """
        self._asr_keep_alive = keep_alive
        logger.info(f"ASR keep_alive set to: {keep_alive}")
    
    def get_asr_keep_alive(self) -> bool:
        """
        Get the current ASR keep_alive state.
        
        Returns:
            True if ASR should be kept alive, False otherwise
        """
        return self._asr_keep_alive
    

# Global singleton instance
_coordinator_instance: Optional[ServiceCoordinator] = None


def get_service_coordinator() -> ServiceCoordinator:
    """Get the global ServiceCoordinator singleton."""
    global _coordinator_instance
    if _coordinator_instance is None:
        _coordinator_instance = ServiceCoordinator()
    return _coordinator_instance
