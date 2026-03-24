#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
LLM Process Manager

Manages LLM subprocess lifecycle and provides execute_request() interface.
Singleton instance for LLM inference delegation.
"""

import threading
from typing import AsyncGenerator, Dict, Any
from pathlib import Path

from openapi_server.impl.genie_wrapper.inference_process_manager import InferenceProcessManager
from openapi_server.impl.genie_wrapper.inference_protocol import InferenceProtocol
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.impl.constant import SAMPLER_CONFIG_PATH
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class LLMProcessManager(InferenceProcessManager):
    """
    LLM process manager singleton.

    Delegates LLM inference to a persistent subprocess.
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
            super().__init__(process_type="llm")
            self._initialized = True

    @classmethod
    def get_instance(cls) -> 'LLMProcessManager':
        """Get singleton instance."""
        return cls()

    def _get_subprocess_script(self) -> Path:
        """Get path to LLM subprocess script."""
        return Path(__file__).parent / "llm_process.py"

    def _create_init_command(self, model_id: str, config_path: str, sampler_config: str) -> Dict[str, Any]:
        """Create INIT command for LLM subprocess."""
        return InferenceProtocol.create_init_command(
            model=model_id,
            config_file=config_path,
            sampler_config=sampler_config,
            streaming=True  # LLM subprocess always created in streaming mode
        )

    def _create_execute_command(
        self,
        event_id: str,
        prompt: str,
        streaming: bool,
        max_tokens: int,
        temperature: float,
        top_p: float,
        top_k: int,
        presence_penalty: float,
        frequency_penalty: float,
        **kwargs
    ) -> Dict[str, Any]:
        """Create EXECUTE command for LLM subprocess."""
        return InferenceProtocol.create_execute_command(
            event_id=event_id,
            prompt=prompt,
            streaming=streaming,
            max_tokens=max_tokens,
            temperature=temperature,
            top_p=top_p,
            top_k=top_k,
            presence_penalty=presence_penalty,
            frequency_penalty=frequency_penalty
        )

    async def execute_request(
        self,
        event_id: str,
        session_id: str,
        model: str,
        prompt: str,
        streaming: bool = True,
        max_tokens: int = 1024,
        temperature: float = 0.7,
        top_p: float = 0.9,
        top_k: int = -1,
        presence_penalty: float = 0.0,
        frequency_penalty: float = 0.0
    ) -> AsyncGenerator[str, None]:
        """
        Execute LLM request and stream tokens.

        Args:
            event_id: Unique event identifier
            session_id: Session identifier
            model: Model ID
            prompt: Text prompt
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

        # Create EXECUTE command
        execute_cmd = self._create_execute_command(
            event_id=event_id,
            prompt=prompt,
            streaming=streaming,
            max_tokens=max_tokens,
            temperature=temperature,
            top_p=top_p,
            top_k=top_k,
            presence_penalty=presence_penalty,
            frequency_penalty=frequency_penalty
        )

        # Execute and yield tokens
        async for token in self._execute_request_internal(event_id, execute_cmd):
            yield token
