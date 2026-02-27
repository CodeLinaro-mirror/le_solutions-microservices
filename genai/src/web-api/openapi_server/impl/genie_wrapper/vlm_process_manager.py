#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM Process Manager

Manages VLM subprocess lifecycle and provides execute_request() interface.
Singleton instance for VLM inference delegation.
"""

import base64
import threading
from typing import AsyncGenerator, Dict, Any, Optional, Tuple
from pathlib import Path

from openapi_server.impl.genie_wrapper.inference_process_manager import InferenceProcessManager
from openapi_server.impl.genie_wrapper.inference_protocol import InferenceProtocol
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.impl.constant import SAMPLER_CONFIG_PATH
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class VLMProcessManager(InferenceProcessManager):
    """
    VLM process manager singleton.

    Delegates VLM inference to a persistent subprocess.
    """

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if not hasattr(self, '_initialized') or not self._initialized:
            super().__init__(process_type="vlm")
            # VLM models take longer to load (vision encoder + LLM)
            self.init_timeout = 300  # 5 minutes
            self._initialized = True

    @classmethod
    def get_instance(cls) -> 'VLMProcessManager':
        """Get singleton instance."""
        return cls()

    def _get_subprocess_script(self) -> Path:
        """Get path to VLM subprocess script."""
        return Path(__file__).parent / "vlm_process.py"

    def _create_init_command(self, model_id: str, config_path: str, sampler_config: str) -> Dict[str, Any]:
        """Create INIT command for VLM subprocess."""
        return InferenceProtocol.create_init_command(
            model=model_id,
            config_file=config_path,
            sampler_config=sampler_config,
            streaming=True  # VLM subprocess always created in streaming mode
        )

    def _send_reset_and_wait(self):
        """
        Override to prevent sending RESET command.
        VLM is standalone by nature and does not support/need KV cache resets.
        """
        logger.info("VLM does not support/need reset. Skipping.")

    def _create_execute_command(
        self,
        event_id: str,
        prompt: str,
        streaming: bool,
        max_tokens: int,
        temperature: float,
        top_p: float,
        presence_penalty: float,
        frequency_penalty: float,
        **kwargs
    ) -> Dict[str, Any]:
        """Create EXECUTE command for VLM subprocess."""
        # Extract VLM-specific parameters
        image_data_b64 = kwargs.get('image_data_b64')
        image_size = kwargs.get('image_size')

        return InferenceProtocol.create_execute_command(
            event_id=event_id,
            prompt=prompt,
            streaming=streaming,
            max_tokens=max_tokens,
            temperature=temperature,
            top_p=top_p,
            presence_penalty=presence_penalty,
            frequency_penalty=frequency_penalty,
            image_data_b64=image_data_b64,
            image_size=image_size
        )

    async def execute_request(
        self,
        event_id: str,
        session_id: str,
        model: str,
        prompt: str,
        image_bytes: Optional[bytes] = None,
        image_size: Optional[Tuple[int, int]] = None,
        streaming: bool = True,
        max_tokens: int = 1024,
        temperature: float = 0.7,
        top_p: float = 0.9,
        presence_penalty: float = 0.0,
        frequency_penalty: float = 0.0
    ) -> AsyncGenerator[str, None]:
        """
        Execute VLM request and stream tokens.

        Args:
            event_id: Unique event identifier
            session_id: Session identifier
            model: Model ID
            prompt: Text prompt
            image_bytes: Image data as bytes (optional)
            image_size: Image size as (width, height) tuple (optional)
            streaming: Whether to stream tokens
            max_tokens: Maximum tokens to generate
            temperature: Sampling temperature
            top_p: Top-p sampling
            presence_penalty: Presence penalty
            frequency_penalty: Frequency penalty

        Yields:
            Generated tokens
        """
        # Get model config path
        config_path = CommonUtils.get_model_config_path(model)
        sampler_config = SAMPLER_CONFIG_PATH

        # Ensure process is running with correct model/session
        self._ensure_process_running(model, config_path, sampler_config, session_id)

        # Encode image data if provided
        image_data_b64 = None
        if image_bytes:
            image_data_b64 = base64.b64encode(image_bytes).decode('utf-8')
            logger.info(f"Event {event_id}: Encoded image data ({len(image_bytes)} bytes -> {len(image_data_b64)} chars)")

        # Create EXECUTE command
        execute_cmd = self._create_execute_command(
            event_id=event_id,
            prompt=prompt,
            streaming=streaming,
            max_tokens=max_tokens,
            temperature=temperature,
            top_p=top_p,
            presence_penalty=presence_penalty,
            frequency_penalty=frequency_penalty,
            image_data_b64=image_data_b64,
            image_size=image_size
        )

        # Execute and yield tokens
        async for token in self._execute_request_internal(event_id, execute_cmd):
            yield token

    async def _execute_request_internal(
        self,
        event_id: str,
        execute_cmd: Dict[str, Any]
    ) -> AsyncGenerator[str, None]:
        """
        Execute request and stream tokens via named pipe.
        """
        import tempfile
        import os
        import json
        import queue
        import asyncio
        from openapi_server.impl.genie_wrapper.inference_protocol import ResponseType

        # Create named pipe
        pipe_path = tempfile.mktemp(suffix='.fifo', prefix='vlm_')
        os.mkfifo(pipe_path)
        logger.info(f"Event {event_id}: Created named pipe: {pipe_path}")

        # Add pipe path to command
        execute_cmd["pipe_path"] = pipe_path

        try:
            # Send EXECUTE command over socket
            self._send_command(execute_cmd)
            logger.info(f"Event {event_id}: Sent EXECUTE command")

            # Create queue for tokens
            token_queue = queue.Queue()

            def read_pipe():
                try:
                    with open(pipe_path, 'r') as pipe:
                        for line in pipe:
                            try:
                                data = json.loads(line.strip())
                                if data['type'] == 'token':
                                    token_queue.put(data['content'])
                                elif data['type'] == 'done':
                                    pass # Done is just informational, socket will send READY
                                elif data['type'] == 'error':
                                    token_queue.put(f"__ERROR__{data['message']}")
                            except json.JSONDecodeError:
                                pass
                except Exception as e:
                    logger.error(f"Event {event_id}: Error reading pipe: {e}")
                    token_queue.put(f"__ERROR__{str(e)}")

            def read_socket():
                try:
                    while True:
                        response = self._read_response(timeout=self.execute_timeout)
                        response_type = response["type"]

                        if response_type == ResponseType.READY.value:
                            logger.debug(f"Event {event_id}: Received READY, stream complete")
                            token_queue.put(None)  # Sentinel
                            break
                        elif response_type == ResponseType.ERROR.value:
                            error_msg = response.get("message", "Unknown error")
                            token_queue.put(f"__ERROR__{error_msg}")
                            token_queue.put(None)
                            break
                        # Ignore TOKEN/DONE over socket since we use pipe
                except Exception as e:
                    logger.error(f"Event {event_id}: Error reading socket: {e}")
                    token_queue.put(f"__ERROR__{str(e)}")
                    token_queue.put(None)

            pipe_thread = threading.Thread(
                target=read_pipe,
                daemon=True,
                name=f"VLMPipeReader-{event_id}"
            )
            socket_thread = threading.Thread(
                target=read_socket,
                daemon=True,
                name=f"VLMSocketReader-{event_id}"
            )

            pipe_thread.start()
            socket_thread.start()

            # Yield tokens from queue
            while True:
                try:
                    token = token_queue.get(timeout=0.1)
                except queue.Empty:
                    if not socket_thread.is_alive() and not pipe_thread.is_alive() and token_queue.empty():
                        break
                    await asyncio.sleep(0.01)
                    continue

                if token is None:
                    break

                if isinstance(token, str) and token.startswith("__ERROR__"):
                    error_msg = token.replace("__ERROR__", "")
                    raise RuntimeError(f"VLM subprocess error: {error_msg}")

                yield token

            socket_thread.join(timeout=2.0)

        except Exception as e:
            logger.error(f"Event {event_id}: Error executing request: {e}", exc_info=True)
            raise
        finally:
            if os.path.exists(pipe_path):
                try:
                    os.unlink(pipe_path)
                except Exception as e:
                    logger.warning(f"Failed to clean up pipe {pipe_path}: {e}")
