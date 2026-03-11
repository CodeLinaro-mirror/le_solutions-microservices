#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Inference Process Manager (Base Class)

Base class for managing inference subprocess lifecycle and IPC communication.
Provides common functionality for LLM and VLM process managers.
"""

import os
import sys
import time
import socket
import subprocess
import threading
import queue
import asyncio
import resource
from typing import Optional, AsyncGenerator, Dict, Any
from pathlib import Path
from abc import ABC, abstractmethod

from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.inference_protocol import InferenceProtocol, CommandType, ResponseType
from openapi_server.managers.system_resource_manager import SystemResourceManager
from openapi_server.managers.model_config_manager import get_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class InferenceProcessManager(ABC):
    """
    Base class for inference process managers.

    Handles:
    - Process lifecycle (start, stop, restart)
    - Socket communication (bidirectional IPC)
    - Log redirection (subprocess stdout → parent logger)
    - Token streaming (background thread + queue → async generator)
    - Model/session switching (restart or reset)
    """

    def __init__(self, process_type: str):
        """
        Initialize inference process manager.

        Args:
            process_type: Type of process ("llm" or "vlm") for logging
        """
        self.process_type = process_type
        self.process: Optional[subprocess.Popen] = None
        self.sock = None  # Socket for bidirectional communication
        self.sock_file_read = None  # Socket file object for reading
        self.sock_file_write = None  # Socket file object for writing
        self.current_model: Optional[str] = None
        self.current_session_id: Optional[str] = None

        # Log reader thread
        self._log_reader_thread: Optional[threading.Thread] = None
        self.last_sdk_error: Optional[str] = None

        # Timeouts (in seconds)
        self.startup_timeout = 10
        self.init_timeout = 60
        self.execute_timeout = 300

        # Resource management
        self._resource_manager = SystemResourceManager()
        self._config_manager = get_config_manager()
        self._process_id: Optional[str] = None  # Unique ID for resource tracking
        self._is_active = False  # Whether process is actively executing

        logger.info(f"{self.process_type.upper()}ProcessManager initialized")

    @abstractmethod
    def _get_subprocess_script(self) -> Path:
        """
        Get path to subprocess script.

        Returns:
            Path to subprocess script (e.g., llm_process.py)
        """
        pass

    @abstractmethod
    def _create_init_command(self, model_id: str, config_path: str, sampler_config: str) -> Dict[str, Any]:
        """
        Create INIT command for subprocess.

        Args:
            model_id: Model identifier
            config_path: Path to model config file
            sampler_config: Path to sampler config file

        Returns:
            INIT command dictionary
        """
        pass

    @abstractmethod
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
        """
        Create EXECUTE command for subprocess.

        Args:
            event_id: Event identifier
            prompt: Text prompt
            streaming: Whether to stream tokens
            max_tokens: Maximum tokens to generate
            temperature: Sampling temperature
            top_p: Top-p sampling
            top_k: Top-k sampling (<=0 uses bundle default)
            presence_penalty: Presence penalty
            frequency_penalty: Frequency penalty
            **kwargs: Additional parameters (e.g., image_data_b64 for VLM)

        Returns:
            EXECUTE command dictionary
        """
        pass

    def _check_memory_and_evict_if_needed(self, model_id: str) -> None:
        """
        Check if sufficient memory is available for the model.
        If not, evict idle processes using LRU policy.

        Args:
            model_id: Model identifier

        Raises:
            RuntimeError: If insufficient memory even after evicting all idle processes
        """
        # Get memory requirement for the model
        memory_mb = self._config_manager.get_memory_requirement_mb(model_id)

        # Check memory availability
        sufficient, available_mb, processes_to_evict = self._resource_manager.check_memory_availability(memory_mb)

        if sufficient and not processes_to_evict:
            # Sufficient memory, no eviction needed
            return

        if not sufficient:
            # Insufficient memory even after evicting all idle processes
            logger.error(f"Insufficient memory for model {model_id} ({memory_mb}MB required, "
                        f"{available_mb}MB available). Cannot proceed even after evicting all idle processes.")
            raise RuntimeError(
                f"Insufficient system memory to load model {model_id}. "
                f"Required: {memory_mb}MB (with headroom), Available: {available_mb}MB. "
                f"Please free up memory or use a smaller model."
            )

        # Evict idle processes
        logger.info(f"Evicting {len(processes_to_evict)} idle processes to free memory for {model_id}")
        for process_id in processes_to_evict:
            try:
                # Get the process manager for this process
                # Process IDs are in format: "llm-{model_name}" or "vlm-{model_name}"
                if process_id.startswith("llm-"):
                    from openapi_server.impl.genie_wrapper.llm_process_manager import LLMProcessManager
                    manager = LLMProcessManager()
                elif process_id.startswith("vlm-"):
                    from openapi_server.impl.genie_wrapper.vlm_process_manager import VLMProcessManager
                    manager = VLMProcessManager()
                else:
                    logger.warning(f"Unknown process ID format: {process_id}")
                    continue

                # Force shutdown the process during eviction
                logger.info(f"Evicting idle process: {process_id}")
                manager.shutdown(force=True)

                # Brief delay between force shutdowns to allow OS to reclaim resources
                time.sleep(0.5)

            except Exception as e:
                logger.error(f"Error evicting process {process_id}: {e}")

    def _register_process(self, model_id: str):
        """
        Register process with resource manager.

        Args:
            model_id: Model identifier
        """
        if self._process_id:
            # Already registered
            return

        # Create unique process ID
        self._process_id = f"{self.process_type}-{model_id}"

        # Get memory requirement
        memory_mb = self._config_manager.get_memory_requirement_mb(model_id)

        # Register with resource manager
        self._resource_manager.register_process(
            process_id=self._process_id,
            model_name=model_id,
            memory_mb=memory_mb,
            is_active=False
        )

        logger.info(f"Registered process {self._process_id} with resource manager ({memory_mb}MB)")

    def _unregister_process(self):
        """Unregister process from resource manager."""
        if self._process_id:
            self._resource_manager.unregister_process(self._process_id)
            logger.info(f"Unregistered process {self._process_id} from resource manager")
            self._process_id = None

    def _mark_active(self):
        """Mark process as actively executing."""
        if self._process_id:
            self._resource_manager.mark_process_active(self._process_id)
            self._is_active = True

    def _mark_idle(self):
        """Mark process as idle (not executing)."""
        if self._process_id:
            self._resource_manager.mark_process_idle(self._process_id)
            self._is_active = False

    def _start_process(self, model_id: str, config_path: str, sampler_config: str):
        """
        Start inference subprocess with specified model.

        Args:
            model_id: Model identifier
            config_path: Path to model config file
            sampler_config: Path to sampler config file
        """
        try:
            logger.info("=" * 60)
            logger.info(f"Starting {self.process_type.upper()} Process")
            logger.info(f"  Model: {model_id}")
            logger.info(f"  Config: {config_path}")
            logger.info("=" * 60)

            # Check memory and evict idle processes if needed
            self._check_memory_and_evict_if_needed(model_id)

            # Get subprocess script path
            script_path = self._get_subprocess_script()
            if not script_path.exists():
                raise FileNotFoundError(f"Subprocess script not found: {script_path}")

            # Create socketpair for bidirectional communication
            logger.info("Creating socketpair for IPC")
            parent_sock, child_sock = socket.socketpair()
            child_fd = child_sock.fileno()

            logger.info(f"Socketpair created: parent_fd={parent_sock.fileno()}, child_fd={child_fd}")

            # Setup environment for child process
            env = os.environ.copy()
            env.update({
                f"{self.process_type.upper()}_SOCKET_FD": str(child_fd),
                "PYTHONUNBUFFERED": "1"  # Ensure logs are flushed immediately
            })

            # Define preexec_fn to set resource limits for child process
            def set_child_limits():
                """
                Set resource limits for the child subprocess.
                This runs after fork() but before exec(), affecting only the child process.
                Raises soft limits to match hard limits set by Docker container.
                """
                try:
                    # Raise RLIMIT_NOFILE (file descriptors) to maximum allowed
                    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
                    if soft < hard:
                        resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
                        logger.debug(f"Child process: Set RLIMIT_NOFILE to {hard}")
                except Exception as e:
                    # Log but don't fail - container limits may already be sufficient
                    logger.warning(f"Could not set RLIMIT_NOFILE for child: {e}")

                try:
                    # Raise RLIMIT_MEMLOCK (locked memory) to maximum allowed
                    soft, hard = resource.getrlimit(resource.RLIMIT_MEMLOCK)
                    if soft < hard:
                        resource.setrlimit(resource.RLIMIT_MEMLOCK, (hard, hard))
                        logger.debug(f"Child process: Set RLIMIT_MEMLOCK to {hard}")
                except Exception as e:
                    logger.warning(f"Could not set RLIMIT_MEMLOCK for child: {e}")

            # Start subprocess with child socket FD passed via pass_fds
            logger.info(f"Starting {self.process_type.upper()} process with socket FD {child_fd}")
            self.process = subprocess.Popen(
                [sys.executable, str(script_path)],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,  # Merge stderr into stdout
                pass_fds=(child_fd,),  # Pass child socket FD to subprocess
                preexec_fn=set_child_limits  # Set resource limits for child process
            )

            logger.info(f"{self.process_type.upper()} process started (PID: {self.process.pid})")

            # Close child socket in parent (child will use it)
            child_sock.close()

            # Wrap parent socket as separate read/write file objects for line-based I/O
            self.sock = parent_sock
            self.sock_file_read = self.sock.makefile('r', buffering=1)
            self.sock_file_write = self.sock.makefile('w', buffering=1)
            logger.info("Parent socket wrapped as read/write file objects")

            # Start log reader thread
            self._start_log_reader()

            # Reset error tracking before initialization
            self.last_sdk_error = None

            # Send INIT command
            init_cmd = self._create_init_command(model_id, config_path, sampler_config)
            self._send_command(init_cmd)

            # Wait for READY response
            response = self._read_response(timeout=self.init_timeout)
            if response["type"] != ResponseType.READY.value:
                err_msg = response.get("message", str(response))
                if self.last_sdk_error:
                    err_msg += f" (SDK Code: {self.last_sdk_error})"
                raise RuntimeError(f"{self.process_type.upper()} process failed to initialize: {err_msg}")

            self.current_model = model_id

            # Register process with resource manager
            self._register_process(model_id)

            logger.info(f"{self.process_type.upper()} process ready for model: {model_id}")

        except Exception as e:
            logger.error(f"Error starting {self.process_type.upper()} process: {e}", exc_info=True)
            self._cleanup_process()
            raise

    def _start_log_reader(self):
        """Start background thread to read subprocess logs."""
        def read_logs():
            import re
            # Only search if it looks like an error to save CPU cycles
            error_pattern = re.compile(r'\b(1002|1003|5000|6000|6001|14001|14003)\b')
            try:
                for line in iter(self.process.stdout.readline, b''):
                    line = line.decode('utf-8', errors='replace').rstrip()
                    if line:
                        logger.info(f"[{self.process_type.upper()}-PROC] {line}")

                        # Capture known SDK error codes if printed
                        line_lower = line.lower()
                        if "err" in line_lower or "error" in line_lower or "fail" in line_lower:
                            match = error_pattern.search(line)
                            if match:
                                self.last_sdk_error = match.group(1)
            except Exception as e:
                logger.error(f"Error reading {self.process_type.upper()} subprocess logs: {e}")

        self._log_reader_thread = threading.Thread(
            target=read_logs,
            daemon=True,
            name=f"{self.process_type.upper()}ProcessLogReader"
        )
        self._log_reader_thread.start()
        logger.info(f"{self.process_type.upper()} log reader thread started")

    def _send_command(self, command: Dict[str, Any]):
        """Send a command to subprocess via socket."""
        if not self.sock_file_write:
            raise RuntimeError("Socket not connected")

        try:
            self.sock_file_write.write(InferenceProtocol.serialize(command))
            self.sock_file_write.flush()
            logger.debug(f"Sent command: {command['type']}")
        except Exception as e:
            logger.error(f"Error sending command: {e}")
            raise

    def _read_response(self, timeout: Optional[float] = None) -> Dict[str, Any]:
        """
        Read a response from subprocess via socket.

        Args:
            timeout: Timeout in seconds (None for blocking)

        Returns:
            Response dictionary

        Raises:
            TimeoutError: If timeout expires
            EOFError: If socket is closed
        """
        if not self.sock_file_read:
            raise RuntimeError("Socket not connected")

        # Set timeout on the socket
        if timeout is not None:
            self.sock.settimeout(timeout)
        else:
            self.sock.settimeout(None)

        try:
            line = self.sock_file_read.readline()
            if not line:
                raise EOFError(f"{self.process_type.upper()} process socket closed")

            response = InferenceProtocol.deserialize(line)
            return response
        except socket.timeout:
            raise TimeoutError(f"Timeout waiting for response ({timeout}s)")
        except Exception as e:
            if isinstance(e, (EOFError, TimeoutError)):
                raise
            logger.error(f"Error reading response: {e}")
            raise

    def _send_reset_and_wait(self):
        """Send RESET command and wait for READY response."""
        try:
            reset_cmd = InferenceProtocol.create_reset_command()
            self._send_command(reset_cmd)

            # Wait for READY response
            response = self._read_response(timeout=10.0)
            if response["type"] != ResponseType.READY.value:
                raise RuntimeError(f"Unexpected response to RESET: {response}")

            logger.info(f"{self.process_type.upper()} handle reset successfully")
        except Exception as e:
            logger.error(f"Error resetting {self.process_type.upper()} handle: {e}")
            raise

    def _cleanup_process(self):
        """Cleanup subprocess and resources."""
        logger.info(f"Cleaning up {self.process_type.upper()} process")

        # Unregister from resource manager
        self._unregister_process()

        try:
            # Close socket read file
            if self.sock_file_read:
                try:
                    self.sock_file_read.close()
                except Exception as e:
                    logger.warning(f"Error closing socket read file: {e}")
                self.sock_file_read = None
        except Exception as e:
            logger.error(f"Error during socket read file cleanup: {e}")

        try:
            # Close socket write file
            if self.sock_file_write:
                try:
                    self.sock_file_write.close()
                except Exception as e:
                    logger.warning(f"Error closing socket write file: {e}")
                self.sock_file_write = None
        except Exception as e:
            logger.error(f"Error during socket write file cleanup: {e}")

        try:
            # Close socket
            if self.sock:
                try:
                    self.sock.close()
                except Exception as e:
                    logger.warning(f"Error closing socket: {e}")
                self.sock = None
        except Exception as e:
            logger.error(f"Error during socket cleanup: {e}")

        try:
            # Terminate process
            if self.process:
                if self.process.poll() is None:
                    logger.info(f"Terminating {self.process_type.upper()} process (PID: {self.process.pid})")
                    self.process.terminate()

                    # Wait for graceful shutdown
                    try:
                        self.process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        logger.warning(f"{self.process_type.upper()} process did not terminate, killing")
                        self.process.kill()
                        self.process.wait()

                # Close stdout pipe to unblock the log reader thread
                try:
                    if self.process.stdout:
                        self.process.stdout.close()
                except Exception as e:
                    logger.warning(f"Error closing process stdout pipe: {e}")

                self.process = None
        except Exception as e:
            logger.error(f"Error during process cleanup: {e}")

        # Wait for log reader thread to finish (it will exit once stdout pipe is closed)
        try:
            if self._log_reader_thread and self._log_reader_thread.is_alive():
                self._log_reader_thread.join(timeout=0.2)
                if self._log_reader_thread.is_alive():
                    logger.warning(f"{self.process_type.upper()} log reader thread did not exit in time")
                else:
                    logger.info(f"{self.process_type.upper()} log reader thread exited cleanly")
            self._log_reader_thread = None
        except Exception as e:
            logger.error(f"Error waiting for log reader thread: {e}")

        self.current_model = None
        self.current_session_id = None

    def _ensure_process_running(self, model_id: str, config_path: str, sampler_config: str, session_id: str):
        """
        Ensure subprocess is running with correct model and for correct session.

        Args:
            model_id: Model identifier
            config_path: Path to model config file
            sampler_config: Path to sampler config file
            session_id: Session identifier
        """
        from openapi_server.impl.constant import ADHOC_MODE

        # Model change → always kill and restart
        if self.current_model and self.current_model != model_id:
            logger.info(f"Model switch: {self.current_model} → {model_id}, restarting subprocess")
            self._cleanup_process()

        # Start process if not running
        if not self.process or self.process.poll() is not None:
            self._start_process(model_id, config_path, sampler_config)
            self.current_session_id = session_id
            return

        # Process is running with correct model
        if ADHOC_MODE:
            # In ADHOC_MODE, the prompt ALWAYS contains the full conversation history.
            # If we don't clear the KV cache, the engine appends the full history to the existing
            # full history, causing exponential token growth and "Context Size Exceeded".
            # Therefore, we MUST send a RESET command before every execution.
            logger.info(f"ADHOC_MODE active: sending RESET to clear KV cache before execution")
            self._send_reset_and_wait()
            self.current_session_id = session_id
        else:
            # Non-ADHOC mode relies on KV cache to remember previous turns.
            # We ONLY send RESET if the user switches to a completely different session,
            # so the new session doesn't inherit the previous session's memory.
            if self.current_session_id and self.current_session_id != session_id:
                logger.info(f"Non-ADHOC session switch: {self.current_session_id} → {session_id}, sending RESET")
                self._send_reset_and_wait()

            self.current_session_id = session_id

    async def _execute_request_internal(
        self,
        event_id: str,
        execute_cmd: Dict[str, Any]
    ) -> AsyncGenerator[str, None]:
        """
        Execute request and stream tokens (internal implementation).

        Args:
            event_id: Event identifier
            execute_cmd: EXECUTE command dictionary

        Yields:
            Generated tokens
        """
        try:
            # Mark process as active
            self._mark_active()

            # Reset error tracking before execution
            self.last_sdk_error = None

            # Send EXECUTE command
            self._send_command(execute_cmd)
            logger.info(f"Event {event_id}: Sent EXECUTE command")

            # Create queue for tokens
            token_queue = queue.Queue()

            # Start background thread to read responses
            def read_responses():
                try:
                    while True:
                        response = self._read_response(timeout=self.execute_timeout)
                        response_type = response["type"]

                        if response_type == ResponseType.TOKEN.value:
                            content = response.get("content", "")
                            if content:
                                token_queue.put(content)
                        elif response_type == ResponseType.DONE.value:
                            logger.debug(f"Event {event_id}: Received DONE, waiting for READY")
                            # Don't break yet, wait for READY
                        elif response_type == ResponseType.READY.value:
                            logger.debug(f"Event {event_id}: Received READY, stream complete")
                            token_queue.put(None)  # Sentinel
                            break
                        elif response_type == ResponseType.ERROR.value:
                            error_msg = response.get("message", "Unknown error")
                            if self.last_sdk_error:
                                error_msg += f" (SDK Code: {self.last_sdk_error})"
                            token_queue.put(f"__ERROR__{error_msg}")
                            token_queue.put(None)
                            break
                except Exception as e:
                    logger.error(f"Error in reader thread: {e}")
                    error_msg = str(e)
                    if self.last_sdk_error:
                        error_msg += f" (SDK Code: {self.last_sdk_error})"
                    token_queue.put(f"__ERROR__{error_msg}")
                    token_queue.put(None)

            reader_thread = threading.Thread(
                target=read_responses,
                daemon=True,
                name=f"{self.process_type.upper()}ResponseReader-{event_id}"
            )
            reader_thread.start()

            # Yield tokens from queue
            while True:
                try:
                    token = token_queue.get(timeout=0.1)
                except queue.Empty:
                    if not reader_thread.is_alive() and token_queue.empty():
                        break
                    await asyncio.sleep(0.01)
                    continue

                if token is None:
                    break

                if isinstance(token, str) and token.startswith("__ERROR__"):
                    error_msg = token.replace("__ERROR__", "")
                    raise RuntimeError(f"{self.process_type.upper()} subprocess error: {error_msg}")

                yield token

            reader_thread.join(timeout=2.0)

        except Exception as e:
            logger.error(f"Event {event_id}: Error executing request: {e}", exc_info=True)
            raise
        finally:
            # Mark process as idle after execution completes
            self._mark_idle()

    def shutdown(self, force: bool = False):
        """Gracefully shutdown subprocess, or force kill if requested."""
        logger.info(f"Shutting down {self.process_type.upper()} process manager (force={force})")

        if force:
            self._cleanup_process()
            return

        try:
            if self.process and self.process.poll() is None:
                # Send SHUTDOWN command
                shutdown_cmd = InferenceProtocol.create_shutdown_command()
                self._send_command(shutdown_cmd)

                # Wait for process to exit
                try:
                    self.process.wait(timeout=5)
                    logger.info(f"{self.process_type.upper()} process shutdown gracefully")
                except subprocess.TimeoutExpired:
                    logger.warning(f"{self.process_type.upper()} process did not shutdown gracefully")
        except Exception as e:
            logger.error(f"Error during shutdown: {e}")
        finally:
            self._cleanup_process()
