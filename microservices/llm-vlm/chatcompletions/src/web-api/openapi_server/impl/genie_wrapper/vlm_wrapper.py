# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM Wrapper - Direct CFFI-based VLM execution with pipeline reuse

This module provides a direct CFFI wrapper for VLM execution, eliminating
subprocess overhead and enabling pipeline reuse across requests.

Key features:
- Direct CFFI calls to libvlmservice.so
- VLM handle caching per model for pipeline reuse
- Thread-safe execution using dedicated VLM thread
- Support for both streaming and non-streaming modes
- Automatic pipeline reset with hardware stabilization delay
"""

import threading
import queue
import time
from typing import Optional, Callable, Dict, Any
from openapi_server.impl.genie_wrapper.vlm_service_singleton import VLMService
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.utils.common_utils import CommonUtils

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class VLMWrapper:
    """
    VLM wrapper providing direct CFFI-based execution with handle caching.

    This class manages VLM handles per model, enabling pipeline reuse across
    requests for improved performance.
    """

    # Class-level handle cache: model_id -> (handle, config_path, last_used_time)
    _handle_cache: Dict[str, tuple] = {}
    _cache_lock = threading.Lock()
    _max_cache_age = 3600  # 1 hour - handles older than this will be recreated

    # Execution thread for thread-safe VLM operations
    _executor_thread: Optional[threading.Thread] = None
    _request_queue: queue.Queue = queue.Queue()
    _running = False
    _executor_lock = threading.Lock()

    @classmethod
    def _start_executor_thread(cls):
        """Start the VLM executor thread if not already running."""
        with cls._executor_lock:
            if cls._executor_thread is None or not cls._executor_thread.is_alive():
                cls._running = True
                cls._executor_thread = threading.Thread(
                    target=cls._executor_run,
                    name="VLMExecutorThread",
                    daemon=True
                )
                cls._executor_thread.start()
                logger.info("VLM executor thread started")

    @classmethod
    def _executor_run(cls):
        """Main executor thread loop."""
        logger.info("VLM executor thread running")

        while cls._running:
            try:
                request = cls._request_queue.get(timeout=1.0)
                request_type = request.get('type')

                if request_type == 'execute':
                    cls._handle_execute(request)
                elif request_type == 'shutdown':
                    logger.info("VLM executor received shutdown request")
                    break
                else:
                    logger.error(f"Unknown request type: {request_type}")

            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"Error in VLM executor thread: {e}", exc_info=True)

        logger.info("VLM executor thread exiting")

    @classmethod
    def _handle_execute(cls, request: Dict[str, Any]):
        """Handle a VLM execution request."""
        try:
            model_id = request['model_id']
            config_path = request['config_path']
            query = request['query']
            streaming = request['streaming']
            callback = request['callback']
            completion_event = request['completion_event']
            result_container = request['result_container']

            # Get VLM service singleton
            vlm_service = VLMService()
            ffi = vlm_service.ffi
            lib = vlm_service.lib

            # Get or create VLM handle
            handle = cls._get_or_create_handle(model_id, config_path, streaming, ffi, lib)

            if handle == ffi.NULL:
                error_msg = f"Failed to get VLM handle for model: {model_id}"
                logger.error(error_msg)
                result_container['error'] = error_msg
                completion_event.set()
                return

            try:
                # Execute VLM completion
                logger.info(f"Executing VLM completion for model={model_id}, streaming={streaming}")
                lib.vlm_chat_completion_create(handle, query, streaming, callback)
                logger.info("VLM completion executed successfully")

                # Note: Pipeline reset with 1-second delay happens automatically in C++ layer

            except Exception as e:
                logger.error(f"Error during VLM execution: {e}", exc_info=True)
                result_container['error'] = str(e)

        except Exception as e:
            logger.error(f"Error in _handle_execute: {e}", exc_info=True)
            result_container['error'] = str(e)
        finally:
            completion_event.set()

    @classmethod
    def _get_or_create_handle(cls, model_id: str, config_path: str, streaming: bool, ffi, lib):
        """
        Get cached VLM handle or create a new one.

        Args:
            model_id: Model identifier
            config_path: Path to model configuration file
            streaming: Whether streaming mode is enabled
            ffi: CFFI instance
            lib: Loaded library

        Returns:
            VLM handle (may be NULL on failure)
        """
        with cls._cache_lock:
            current_time = time.time()
            cache_key = model_id

            # Check if we have a valid cached handle
            if cache_key in cls._handle_cache:
                handle, cached_config, last_used = cls._handle_cache[cache_key]

                # Check if handle is still valid (not too old, same config)
                if (current_time - last_used < cls._max_cache_age and
                    cached_config == config_path):
                    logger.info(f"Reusing cached VLM handle for {cache_key}")
                    cls._handle_cache[cache_key] = (handle, cached_config, current_time)
                    return handle
                else:
                    # Handle is stale, destroy it
                    logger.info(f"Cached handle for {cache_key} is stale, recreating")
                    try:
                        lib.vlm_destroy_object(handle)
                    except Exception as e:
                        logger.warning(f"Error destroying stale handle: {e}")
                    del cls._handle_cache[cache_key]

            # Create new handle
            logger.info(f"Creating new VLM handle for model={model_id}, config={config_path}, streaming={streaming}")

            # Properly encode strings with null termination
            model_bytes = model_id.encode('utf-8') + b'\0'
            config_bytes = config_path.encode('utf-8') + b'\0'
            sampler_path = "/iot-user/app/site-packages/sampler.json"
            sampler_bytes = sampler_path.encode('utf-8') + b'\0'

            model_str = ffi.new("char[]", model_bytes)
            config_str = ffi.new("char[]", config_bytes)
            sampler_str = ffi.new("char[]", sampler_bytes)

            handle = lib.vlm_create_object(model_str, config_str, sampler_str, streaming)

            if handle != ffi.NULL:
                # Cache the handle
                cls._handle_cache[cache_key] = (handle, config_path, current_time)
                logger.info(f"VLM handle created and cached for {cache_key}")
            else:
                logger.error(f"Failed to create VLM handle for {cache_key}")

            return handle

    @classmethod
    def destroy_handle(cls, model_id: str):
        """Explicitly destroy a cached handle for a specific model."""
        with cls._cache_lock:
            # Check if handle exists for this model_id
            if model_id in cls._handle_cache:
                handle, _, _ = cls._handle_cache[model_id]
                logger.info(f"Destroying cached VLM handle for {model_id}")
                try:
                    vlm_service = VLMService()
                    vlm_service.lib.vlm_destroy_object(handle)
                except Exception as e:
                    logger.warning(f"Error destroying VLM handle for {model_id}: {e}")
                del cls._handle_cache[model_id]

    @classmethod
    def execute_vlm_completion(
        cls,
        model_id: str,
        config_path: str,
        query,
        streaming: bool,
        callback,
        timeout: float = 300.0
    ) -> Optional[Dict[str, Any]]:
        """
        Execute a VLM completion request.

        Args:
            model_id: Model identifier
            config_path: Path to model configuration file
            query: CFFI Query structure
            streaming: Whether to use streaming mode
            callback: CFFI callback function
            timeout: Maximum time to wait for completion (seconds)

        Returns:
            Result dictionary or None on error
        """
        # Start executor thread if needed
        cls._start_executor_thread()

        # Create completion event and result container
        completion_event = threading.Event()
        result_container = {'error': None}

        # Submit execution request
        request = {
            'type': 'execute',
            'model_id': model_id,
            'config_path': config_path,
            'query': query,
            'streaming': streaming,
            'callback': callback,
            'completion_event': completion_event,
            'result_container': result_container
        }

        logger.info(f"Submitting VLM execution request for model={model_id}, streaming={streaming}")
        cls._request_queue.put(request)

        # Wait for completion
        if not completion_event.wait(timeout):
            error_msg = f"VLM execution timeout after {timeout} seconds"
            logger.error(error_msg)
            return {'error': error_msg}

        # Check for errors
        if result_container['error']:
            logger.error(f"VLM execution failed: {result_container['error']}")
            return {'error': result_container['error']}

        return result_container

    @classmethod
    def clear_cache(cls):
        """Clear all cached VLM handles."""
        with cls._cache_lock:
            if not cls._handle_cache:
                logger.info("VLM handle cache is already empty")
                return

            logger.info(f"Clearing {len(cls._handle_cache)} cached VLM handles")

            try:
                vlm_service = VLMService()
                lib = vlm_service.lib

                for cache_key, (handle, _, _) in cls._handle_cache.items():
                    try:
                        logger.info(f"Destroying cached handle for {cache_key}")
                        lib.vlm_destroy_object(handle)
                    except Exception as e:
                        logger.warning(f"Error destroying handle for {cache_key}: {e}")

            except Exception as e:
                logger.error(f"Error accessing VLM service during cache clear: {e}")

            cls._handle_cache.clear()
            logger.info("VLM handle cache cleared")

    @classmethod
    def shutdown(cls):
        """Shutdown the VLM executor thread and clear cache."""
        logger.info("Shutting down VLM wrapper")

        # Stop executor thread
        with cls._executor_lock:
            if cls._executor_thread and cls._executor_thread.is_alive():
                cls._running = False
                cls._request_queue.put({'type': 'shutdown'})
                cls._executor_thread.join(timeout=10)

                if cls._executor_thread.is_alive():
                    logger.warning("VLM executor thread did not exit cleanly")

        # Clear handle cache
        cls.clear_cache()

        logger.info("VLM wrapper shutdown complete")
