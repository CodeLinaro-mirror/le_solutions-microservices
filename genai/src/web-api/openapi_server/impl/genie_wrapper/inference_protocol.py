#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Inference Protocol

Common JSON Lines protocol for LLM and VLM subprocess communication.
Uses bidirectional socket for IPC between parent and subprocess.
"""

import json
from typing import Dict, Any, Optional
from enum import Enum


class CommandType(Enum):
    """Command types sent from parent to subprocess."""
    INIT = "INIT"
    EXECUTE = "EXECUTE"
    RESET = "RESET"
    SHUTDOWN = "SHUTDOWN"


class ResponseType(Enum):
    """Response types sent from subprocess to parent."""
    READY = "READY"
    TOKEN = "TOKEN"
    DONE = "DONE"
    ERROR = "ERROR"


class InferenceProtocol:
    """Helper class for protocol message serialization/deserialization."""

    @staticmethod
    def create_init_command(model: str, config_file: str, sampler_config: str, streaming: bool) -> Dict[str, Any]:
        """
        Create INIT command to initialize inference pipeline.

        Args:
            model: Model identifier
            config_file: Full path to model config file
            sampler_config: Full path to sampler config file
            streaming: Whether to enable streaming mode

        Returns:
            INIT command dictionary
        """
        return {
            "type": CommandType.INIT.value,
            "model": model,
            "config_file": config_file,
            "sampler_config": sampler_config,
            "streaming": streaming
        }

    @staticmethod
    def create_execute_command(
        event_id: str,
        prompt: str,
        streaming: bool,
        max_tokens: int = 1024,
        temperature: float = 0.7,
        top_p: float = 0.9,
        presence_penalty: float = 0.0,
        frequency_penalty: float = 0.0,
        image_data_b64: Optional[str] = None,
        image_size: Optional[int] = None,
        pipe_path: Optional[str] = None
    ) -> Dict[str, Any]:
        """
        Create EXECUTE command to run inference.

        Args:
            event_id: Unique event identifier
            prompt: Text prompt
            streaming: Whether to stream tokens
            max_tokens: Maximum tokens to generate
            temperature: Sampling temperature
            top_p: Top-p sampling parameter
            presence_penalty: Presence penalty
            frequency_penalty: Frequency penalty
            image_data_b64: Base64-encoded image data (VLM only)
            image_size: Size of image data in bytes (VLM only)

        Returns:
            EXECUTE command dictionary
        """
        cmd = {
            "type": CommandType.EXECUTE.value,
            "event_id": event_id,
            "prompt": prompt,
            "streaming": streaming,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "top_p": top_p,
            "presence_penalty": presence_penalty,
            "frequency_penalty": frequency_penalty
        }

        # Add image data for VLM
        if image_data_b64 is not None:
            cmd["image_data_b64"] = image_data_b64
            cmd["image_size"] = image_size

        if pipe_path is not None:
            cmd["pipe_path"] = pipe_path

        return cmd

    @staticmethod
    def create_reset_command() -> Dict[str, Any]:
        """
        Create RESET command to reset inference handle (clear KV cache).

        Returns:
            RESET command dictionary
        """
        return {
            "type": CommandType.RESET.value
        }

    @staticmethod
    def create_shutdown_command() -> Dict[str, Any]:
        """
        Create SHUTDOWN command to gracefully shutdown subprocess.

        Returns:
            SHUTDOWN command dictionary
        """
        return {
            "type": CommandType.SHUTDOWN.value
        }

    @staticmethod
    def create_ready_response(event_id: Optional[str] = None) -> Dict[str, Any]:
        """
        Create READY response indicating subprocess is ready.

        Args:
            event_id: Event identifier (optional, for post-execution READY)

        Returns:
            READY response dictionary
        """
        response = {
            "type": ResponseType.READY.value
        }
        if event_id:
            response["event_id"] = event_id
        return response

    @staticmethod
    def create_token_response(event_id: str, content: str) -> Dict[str, Any]:
        """
        Create TOKEN response with generated token.

        Args:
            event_id: Event identifier
            content: Generated token content

        Returns:
            TOKEN response dictionary
        """
        return {
            "type": ResponseType.TOKEN.value,
            "event_id": event_id,
            "content": content
        }

    @staticmethod
    def create_done_response(event_id: str, finish_reason: str) -> Dict[str, Any]:
        """
        Create DONE response indicating completion.

        Args:
            event_id: Event identifier
            finish_reason: Reason for completion (e.g., "stop")

        Returns:
            DONE response dictionary
        """
        return {
            "type": ResponseType.DONE.value,
            "event_id": event_id,
            "finish_reason": finish_reason
        }

    @staticmethod
    def create_error_response(event_id: Optional[str], message: str) -> Dict[str, Any]:
        """
        Create ERROR response indicating an error occurred.

        Args:
            event_id: Event identifier (if available)
            message: Error message

        Returns:
            ERROR response dictionary
        """
        return {
            "type": ResponseType.ERROR.value,
            "event_id": event_id,
            "message": message
        }

    @staticmethod
    def serialize(message: Dict[str, Any]) -> str:
        """
        Serialize message to JSON line format.

        Args:
            message: Message dictionary

        Returns:
            JSON string with newline
        """
        return json.dumps(message) + '\n'

    @staticmethod
    def deserialize(line: str) -> Dict[str, Any]:
        """
        Deserialize JSON line to message dictionary.

        Args:
            line: JSON line string

        Returns:
            Message dictionary

        Raises:
            json.JSONDecodeError: If line is not valid JSON
        """
        return json.loads(line.strip())

    @staticmethod
    def is_command(message: Dict[str, Any]) -> bool:
        """Check if message is a command."""
        msg_type = message.get("type", "")
        return msg_type in [ct.value for ct in CommandType]

    @staticmethod
    def is_response(message: Dict[str, Any]) -> bool:
        """Check if message is a response."""
        msg_type = message.get("type", "")
        return msg_type in [rt.value for rt in ResponseType]

    @staticmethod
    def get_message_type(message: Dict[str, Any]) -> str:
        """Get message type."""
        return message.get("type", "UNKNOWN")
