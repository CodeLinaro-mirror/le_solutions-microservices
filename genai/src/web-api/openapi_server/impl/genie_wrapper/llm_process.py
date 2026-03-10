#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
LLM Process

Standalone process that manages LLM pipeline lifecycle.
Communicates with parent via socket (socketpair).
Keeps pipeline alive between requests for performance.
"""

import os
import sys
import time
import socket
import logging
from typing import Optional
from cffi import FFI

# Setup logging to stdout (captured by parent)
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger("llm_process")

# Import protocol
try:
    from openapi_server.impl.genie_wrapper.inference_protocol import InferenceProtocol, CommandType, ResponseType
    from openapi_server.utils.common_utils import CommonUtils
    from openapi_server.impl.constant import LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST
except ImportError:
    # Adjust path if needed
    repo_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../../'))
    sys.path.insert(0, repo_path)
    from openapi_server.impl.genie_wrapper.inference_protocol import InferenceProtocol, CommandType, ResponseType
    from openapi_server.utils.common_utils import CommonUtils
    from openapi_server.impl.constant import LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST


class LLMProcess:
    """
    LLM Process that keeps pipeline alive between requests.

    Handles:
    - Pipeline initialization and lifecycle
    - Command processing from parent
    - Token streaming back to parent
    - Pipeline reset
    """

    def __init__(self, socket_fd: Optional[int] = None):
        self.socket_fd = socket_fd
        self.sock = None
        self.sock_file_read = None  # Socket file for reading
        self.sock_file_write = None  # Socket file for writing
        self.running = False

        # LLM state
        self.ffi: Optional[FFI] = None
        self.lib = None
        self.llm_handle = None
        self.current_model: Optional[str] = None
        self.streaming_mode: bool = True

    def start(self):
        """Start the LLM process."""
        logger.info("=" * 60)
        logger.info("LLM Process Starting")
        logger.info("=" * 60)

        try:
            # Setup socket communication
            self._setup_socket()

            # Main command loop
            self.running = True
            self._command_loop()

        except Exception as e:
            logger.error(f"Fatal error in LLM process: {e}", exc_info=True)
            sys.exit(1)
        finally:
            self._cleanup()

    def _setup_socket(self):
        """Setup socket communication from inherited file descriptor."""
        logger.info("Setting up socket communication")

        if self.socket_fd is None:
            raise RuntimeError("Socket FD not provided")

        logger.info(f"Using inherited socket FD: {self.socket_fd}")

        # Create socket from inherited file descriptor
        self.sock = socket.socket(fileno=self.socket_fd)

        # Wrap socket as separate read/write file objects for line-based I/O
        self.sock_file_read = self.sock.makefile('r', buffering=1)
        self.sock_file_write = self.sock.makefile('w', buffering=1)

        logger.info("Socket communication established")

    def _command_loop(self):
        """Main command processing loop."""
        logger.info("Entering command loop")

        while self.running:
            try:
                # Read command from socket
                command = self._read_command()
                if command is None:
                    logger.warning("Received None command, exiting")
                    break

                cmd_type = command.get("type")
                logger.info(f"Received command: {cmd_type}")

                # Process command
                if cmd_type == CommandType.INIT.value:
                    self._handle_init(command)

                elif cmd_type == CommandType.EXECUTE.value:
                    self._handle_execute(command)

                elif cmd_type == CommandType.RESET.value:
                    self._handle_reset(command)

                elif cmd_type == CommandType.SHUTDOWN.value:
                    logger.info("Received SHUTDOWN command")
                    self.running = False
                    break

                else:
                    logger.error(f"Unknown command type: {cmd_type}")
                    self._send_error(None, f"Unknown command type: {cmd_type}")

            except EOFError:
                logger.info("Socket closed by parent, exiting")
                break
            except Exception as e:
                logger.error(f"Error processing command: {e}", exc_info=True)
                self._send_error(None, str(e))

    def _read_command(self) -> Optional[dict]:
        """Read a command from the socket."""
        line = self.sock_file_read.readline()
        if not line:
            return None
        return InferenceProtocol.deserialize(line)

    def _send_response(self, response: dict):
        """Send a response to the socket."""
        self.sock_file_write.write(InferenceProtocol.serialize(response))
        self.sock_file_write.flush()

    def _send_error(self, event_id: Optional[str], message: str):
        """Send an error response."""
        response = InferenceProtocol.create_error_response(event_id, message)
        self._send_response(response)

    def _handle_init(self, command: dict):
        """Handle INIT command - initialize LLM pipeline."""
        try:
            model = command["model"]
            config_file = command["config_file"]
            sampler_config = command["sampler_config"]
            streaming = command["streaming"]

            logger.info(f"Initializing LLM pipeline")
            logger.info(f"  Model: {model}")
            logger.info(f"  Config: {config_file}")
            logger.info(f"  Sampler: {sampler_config}")
            logger.info(f"  Streaming: {streaming}")

            # Get library and header paths from environment
            lib_path = os.environ.get("LLM_LIBRARY_PATH")
            header_path = os.environ.get("GENAI_INTERFACE_FILE")

            if not lib_path or not os.path.exists(lib_path):
                # Fallback
                venv_path = os.path.dirname(os.path.dirname(sys.executable))
                lib_path = os.path.join(venv_path, "lib", "libllmservice.so")
                if not os.path.exists(lib_path):
                    raise RuntimeError(f"LLM_LIBRARY_PATH not set or file not found: {lib_path}")

            if not header_path or not os.path.exists(header_path):
                # Fallback
                venv_path = os.path.dirname(os.path.dirname(sys.executable))
                header_path = os.path.join(venv_path, "include", "genai_interface.h")
                if not os.path.exists(header_path):
                    raise RuntimeError(f"GENAI_INTERFACE_FILE not set or file not found: {header_path}")

            logger.info(f"  Library: {lib_path}")
            logger.info(f"  Header: {header_path}")

            # Initialize CFFI
            self.ffi = FFI()
            with open(header_path, 'r') as f:
                header_content = f.read()

            # Update interface definition (same as gen_ai_service_singleton.py)
            updated_interface = header_content.replace(
                'LLMHandle llm_create_object(const char* model, bool streaming);',
                'LLMHandle llm_create_object(const char* model, const char* config_path, const char* sampler_config_path, bool streaming);'
            )
            updated_interface = updated_interface.replace(
                'LLMHandle llm_create_object(const char* model, char* config_path, bool streaming);',
                'LLMHandle llm_create_object(const char* model, char* config_path, const char* sampler_config_path, bool streaming);'
            )
            if 'bool llm_is_initialized' not in updated_interface:
                updated_interface += """
bool llm_is_initialized(LLMHandle handle);
const char* llm_get_last_error(LLMHandle handle);
bool vlm_is_initialized(VLMHandle handle);
const char* vlm_get_last_error(VLMHandle handle);
"""
            if 'void llm_reset_object' not in updated_interface:
                updated_interface += """
void llm_reset_object(LLMHandle handle);
"""
            self.ffi.cdef(updated_interface)

            # Load library
            self.lib = self.ffi.dlopen(lib_path)
            logger.info("CFFI initialized, library loaded")

            # Create LLM object
            model_input = self.ffi.new("char[]", model.encode('utf-8'))
            config_path_input = self.ffi.new("char[]", config_file.encode('utf-8'))
            sampler_input = self.ffi.new("char[]", sampler_config.encode('utf-8'))

            logger.info("Creating LLM handle...")
            self.llm_handle = self.lib.llm_create_object(model_input, config_path_input, sampler_input, streaming)

            if self.llm_handle == self.ffi.NULL:
                raise RuntimeError(f"Failed to create LLM handle (NULL returned)")

            self.current_model = model
            self.streaming_mode = streaming

            logger.info("LLM pipeline initialized successfully")

            # Send READY response
            response = InferenceProtocol.create_ready_response()
            self._send_response(response)
            logger.info("Sent READY response")

        except Exception as e:
            logger.error(f"Error initializing LLM: {e}", exc_info=True)
            self._send_error(None, f"Initialization failed: {e}")

    def _handle_execute(self, command: dict):
        """Handle EXECUTE command - run LLM inference."""
        event_id = command.get("event_id")

        try:
            if not self.llm_handle:
                raise RuntimeError("LLM not initialized")

            # Extract parameters
            prompt = command["prompt"]
            streaming = command["streaming"]
            max_tokens = command.get("max_tokens", 1024)
            temperature = command.get("temperature", 0.7)
            top_p = command.get("top_p", 0.9)
            top_k = command.get("top_k", -1)
            presence_penalty = command.get("presence_penalty", 0.0)
            frequency_penalty = command.get("frequency_penalty", 0.0)

            logger.info(f"[{event_id}] Executing LLM request")
            logger.info(f"[{event_id}]   Streaming: {streaming}")
            logger.info(f"[{event_id}]   Sampling: temp={temperature}, top_p={top_p}, top_k={top_k}")

            # Create query structure
            query = self.ffi.new(LLMServiceKeys.QUERY)
            if query == self.ffi.NULL:
                raise RuntimeError("Failed to allocate Query pointer")

            # Populate query
            CommonUtils.copy_py_string_to_c_array(self.ffi, query.model, self.current_model, QUERY_CONST.MODEL_STR_MAX_SIZE)
            CommonUtils.copy_py_string_to_c_array(self.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE)
            CommonUtils.copy_py_string_to_c_array(self.ffi, query.message.content, prompt, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

            # Set parameters
            CommonUtils.copy_py_int_to_c_field(self.ffi, query, 'max_completion_tokens', max_tokens)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'temperature', temperature)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'top_p', top_p)
            CommonUtils.copy_py_int_to_c_field(self.ffi, query, 'top_k', top_k)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'presence_penalty', presence_penalty)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'frequency_penalty', frequency_penalty)

            logger.info(f"[{event_id}] Query prepared, executing LLM...")

            # Define callback
            @self.ffi.callback("void(const Response *)")
            def callback(response_ptr):
                try:
                    resp = response_ptr[0]
                    choice = resp.choices[0]
                    msg = choice.message

                    content = self.ffi.string(msg.content).decode("utf-8")
                    finish_reason = self.ffi.string(choice.finish_reason).decode("utf-8")

                    if finish_reason == "error":
                        self._send_error(event_id, content)
                    else:
                        # Send token response
                        if content:
                            token_response = InferenceProtocol.create_token_response(event_id, content)
                            self._send_response(token_response)

                        # Send done if finished
                        if finish_reason == "stop":
                            done_response = InferenceProtocol.create_done_response(event_id, finish_reason)
                            self._send_response(done_response)

                except Exception as e:
                    logger.error(f"[{event_id}] Error in callback: {e}", exc_info=True)

            # Execute LLM completion (blocking)
            self.lib.llm_chat_completion_create(self.llm_handle, query, streaming, callback)

            logger.info(f"[{event_id}] LLM execution completed")

            # Send READY for next request
            ready_response = InferenceProtocol.create_ready_response(event_id=event_id)
            self._send_response(ready_response)
            logger.info(f"[{event_id}] Ready for next request")

        except Exception as e:
            logger.error(f"[{event_id}] Error executing LLM: {e}", exc_info=True)
            self._send_error(event_id, str(e))

    def _handle_reset(self, command: dict):
        """Handle RESET command - reset LLM handle to clear KV cache."""
        try:
            if self.llm_handle:
                self.lib.llm_reset_object(self.llm_handle)
                logger.info("LLM handle reset - KV cache cleared")

            response = InferenceProtocol.create_ready_response()
            self._send_response(response)
        except Exception as e:
            logger.error(f"Error resetting LLM handle: {e}")
            self._send_error(None, f"Reset failed: {e}")

    def _cleanup(self):
        """Cleanup resources."""
        logger.info("Cleaning up LLM process")

        try:
            # Destroy LLM handle
            if self.llm_handle and self.lib:
                logger.info("Destroying LLM handle")
                self.lib.llm_destroy_object(self.llm_handle)
                self.llm_handle = None
        except Exception as e:
            logger.error(f"Error destroying LLM handle: {e}")

        try:
            # Close socket read file
            if self.sock_file_read:
                logger.info("Closing socket read file")
                self.sock_file_read.close()
                self.sock_file_read = None
        except Exception as e:
            logger.error(f"Error closing socket read file: {e}")

        try:
            # Close socket write file
            if self.sock_file_write:
                logger.info("Closing socket write file")
                self.sock_file_write.close()
                self.sock_file_write = None
        except Exception as e:
            logger.error(f"Error closing socket write file: {e}")

        try:
            # Close socket
            if self.sock:
                logger.info("Closing socket")
                self.sock.close()
                self.sock = None
        except Exception as e:
            logger.error(f"Error closing socket: {e}")

        logger.info("LLM process cleanup complete")


def main():
    """Main entry point."""
    # Get socket FD from environment variable
    socket_fd_str = os.environ.get("LLM_SOCKET_FD")
    if not socket_fd_str:
        logger.error("LLM_SOCKET_FD environment variable not set")
        sys.exit(1)

    try:
        socket_fd = int(socket_fd_str)
    except ValueError:
        logger.error(f"Invalid LLM_SOCKET_FD value: {socket_fd_str}")
        sys.exit(1)

    logger.info("Starting LLM Process")
    logger.info(f"Socket FD: {socket_fd}")
    logger.info(f"PID: {os.getpid()}")

    process = LLMProcess(socket_fd=socket_fd)
    process.start()

    logger.info("LLM Process exiting")


if __name__ == "__main__":
    main()
