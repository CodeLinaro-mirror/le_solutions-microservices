#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VLM Process

Standalone process that manages VLM pipeline lifecycle.
Communicates with server via socket.
Keeps pipeline alive between requests for performance.
"""

import os
import sys
import base64
import socket
import logging
from typing import Optional
from cffi import FFI

# Setup logging to separate file
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.FileHandler('vlm_process.log'),
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger("vlm_process")

# Import protocol and utils
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


class VLMProcess:
    """
    VLM Process that keeps pipeline alive between requests.

    Handles:
    - Pipeline initialization and lifecycle
    - Command processing from server
    - Token streaming back to server
    - Pipeline reset
    - Image data handling
    """

    def __init__(self, socket_fd: Optional[int] = None):
        self.socket_fd = socket_fd
        self.sock = None
        self.sock_file_read = None  # Socket file for reading
        self.sock_file_write = None  # Socket file for writing
        self.running = False

        # VLM state
        self.ffi: Optional[FFI] = None
        self.lib = None
        self.vlm_handle = None
        self.current_model: Optional[str] = None
        self.streaming_mode: bool = True

    def start(self):
        """Start the VLM process."""
        logger.info("=" * 60)
        logger.info("VLM Process Starting")
        logger.info("=" * 60)

        try:
            # Setup socket communication
            self._setup_socket()

            # Main command loop
            self.running = True
            self._command_loop()

        except Exception as e:
            logger.error(f"Fatal error in VLM process: {e}", exc_info=True)
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
                logger.info("Socket closed by server, exiting")
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
        """Handle INIT command - initialize VLM pipeline."""
        try:
            model = command["model"]
            config_file = command["config_file"]
            sampler_config = command["sampler_config"]
            streaming = command["streaming"]

            logger.info(f"Initializing VLM pipeline")
            logger.info(f"  Model: {model}")
            logger.info(f"  Config: {config_file}")
            logger.info(f"  Sampler: {sampler_config}")
            logger.info(f"  Streaming: {streaming}")

            # Get library and header paths from environment
            lib_path = os.environ.get("VLM_LIBRARY_PATH")
            header_path = os.environ.get("GENAI_INTERFACE_FILE")

            if not lib_path or not os.path.exists(lib_path):
                # Fallback
                venv_path = os.path.dirname(os.path.dirname(sys.executable))
                lib_path = os.path.join(venv_path, "lib", "libvlmservice.so")
                if not os.path.exists(lib_path):
                    raise RuntimeError(f"VLM_LIBRARY_PATH not set or file not found: {lib_path}")

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

            # Update interface definition (similar to LLM)
            updated_interface = header_content.replace(
                'VLMHandle vlm_create_object(const char* model, bool streaming);',
                'VLMHandle vlm_create_object(const char* model, const char* config_path, const char* sampler_config_path, bool streaming);'
            )
            updated_interface = updated_interface.replace(
                'VLMHandle vlm_create_object(const char* model, char* config_path, bool streaming);',
                'VLMHandle vlm_create_object(const char* model, char* config_path, const char* sampler_config_path, bool streaming);'
            )
            if 'bool vlm_is_initialized' not in updated_interface:
                updated_interface += """
bool vlm_is_initialized(VLMHandle handle);
const char* vlm_get_last_error(VLMHandle handle);
"""
            self.ffi.cdef(updated_interface)

            # Load library
            self.lib = self.ffi.dlopen(lib_path)
            logger.info("CFFI initialized, library loaded")

            # Create VLM object
            model_input = self.ffi.new("char[]", model.encode('utf-8'))
            config_path_input = self.ffi.new("char[]", config_file.encode('utf-8'))
            sampler_input = self.ffi.new("char[]", sampler_config.encode('utf-8'))

            logger.info("Creating VLM handle...")
            self.vlm_handle = self.lib.vlm_create_object(model_input, config_path_input, sampler_input, streaming)

            if self.vlm_handle == self.ffi.NULL:
                raise RuntimeError(f"Failed to create VLM handle (NULL returned)")

            self.current_model = model
            self.streaming_mode = streaming

            logger.info("VLM pipeline initialized successfully")

            # Send READY response
            response = InferenceProtocol.create_ready_response()
            self._send_response(response)
            logger.info("Sent READY response")

        except Exception as e:
            logger.error(f"Error initializing VLM: {e}", exc_info=True)
            self._send_error(None, f"Initialization failed: {e}")

    def _handle_execute(self, command: dict):
        """Handle EXECUTE command - run VLM inference."""
        event_id = command.get("event_id")

        try:
            if not self.vlm_handle:
                raise RuntimeError("VLM not initialized")

            # Extract parameters
            prompt = command["prompt"]
            streaming = command["streaming"]
            max_tokens = command.get("max_tokens", 1024)
            temperature = command.get("temperature", 0.7)
            top_p = command.get("top_p", 0.9)
            top_k = command.get("top_k", -1)
            presence_penalty = command.get("presence_penalty", 0.0)
            frequency_penalty = command.get("frequency_penalty", 0.0)
            pipe_path = command.get("pipe_path")

            # Extract image data
            image_data_b64 = command.get("image_data_b64")
            image_size = command.get("image_size")

            logger.info(f"[{event_id}] Executing VLM request")
            logger.info(f"[{event_id}]   Streaming: {streaming}")
            logger.info(f"[{event_id}]   Has image: {image_data_b64 is not None}")
            logger.info(f"[{event_id}]   Pipe path: {pipe_path}")
            logger.info(f"[{event_id}]   Sampling: temp={temperature}, top_p={top_p}, top_k={top_k}")

            # Decode image data if provided
            image_data = None
            if image_data_b64:
                image_data = base64.b64decode(image_data_b64)
                logger.info(f"[{event_id}]   Image size: {len(image_data)} bytes")

            # Create query structure
            query = self.ffi.new(LLMServiceKeys.QUERY)
            if query == self.ffi.NULL:
                raise RuntimeError("Failed to allocate Query pointer")

            # Populate query
            CommonUtils.copy_py_string_to_c_array(self.ffi, query.model, self.current_model, QUERY_CONST.MODEL_STR_MAX_SIZE)
            CommonUtils.copy_py_string_to_c_array(self.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE)

            # Set multimodal mode (use_content_items instead of simple content string)
            query.message.use_content_items = True
            query.message.content_items_count = 0

            # Add text content item
            query.message.content_items[0].type = 0  # CONTENT_TYPE_TEXT
            CommonUtils.copy_py_string_to_c_array(
                self.ffi,
                query.message.content_items[0].text,
                prompt,
                12300  # MAX_CONTENT_LENGTH
            )
            query.message.content_items_count = 1

            # Set image data if provided
            if image_data:
                # Add image buffer as second content item
                image_buffer = self.ffi.new("char[]", len(image_data))
                self.ffi.memmove(image_buffer, image_data, len(image_data))

                idx = query.message.content_items_count
                query.message.content_items[idx].type = 1  # CONTENT_TYPE_IMAGE_BUFFER
                query.message.content_items[idx].image.buffer = image_buffer
                query.message.content_items[idx].image.size = len(image_data)
                query.message.content_items_count += 1

            # Set parameters
            CommonUtils.copy_py_int_to_c_field(self.ffi, query, 'max_completion_tokens', max_tokens)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'temperature', temperature)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'top_p', top_p)
            CommonUtils.copy_py_int_to_c_field(self.ffi, query, 'top_k', top_k)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'presence_penalty', presence_penalty)
            CommonUtils.copy_py_float_to_c_field(self.ffi, query, 'frequency_penalty', frequency_penalty)

            logger.info(f"[{event_id}] Query prepared, executing VLM...")

            # Open output pipe if provided
            pipe_handle = None
            if pipe_path:
                try:
                    pipe_handle = open(pipe_path, 'w', buffering=1)  # Line buffered
                    logger.info(f"[{event_id}] Opened output pipe: {pipe_path}")
                except Exception as e:
                    logger.error(f"[{event_id}] Failed to open output pipe: {e}")
                    raise RuntimeError(f"Failed to open output pipe: {e}")

            import threading
            completion_event = threading.Event()
            execution_error = None
            import json

            # Define callback
            @self.ffi.callback("void(const Response *)")
            def callback(response_ptr):
                nonlocal execution_error
                try:
                    resp = response_ptr[0]
                    choice = resp.choices[0]
                    msg = choice.message

                    content = self.ffi.string(msg.content).decode("utf-8")
                    finish_reason = self.ffi.string(choice.finish_reason).decode("utf-8")

                    if pipe_handle:
                        # Write token to pipe
                        pipe_handle.write(json.dumps({
                            "type": "token",
                            "content": content
                        }) + '\n')
                        pipe_handle.flush()

                        if finish_reason == "stop":
                            pipe_handle.write(json.dumps({
                                "type": "done",
                                "finish_reason": finish_reason
                            }) + '\n')
                            pipe_handle.flush()
                            completion_event.set()
                    else:
                        # Fallback to socket if no pipe (for backward compatibility if needed)
                        if content:
                            token_response = InferenceProtocol.create_token_response(event_id, content)
                            self._send_response(token_response)

                        if finish_reason == "stop":
                            done_response = InferenceProtocol.create_done_response(event_id, finish_reason)
                            self._send_response(done_response)
                            completion_event.set()

                except Exception as e:
                    logger.error(f"[{event_id}] Error in callback: {e}", exc_info=True)
                    execution_error = e
                    if pipe_handle:
                        try:
                            pipe_handle.write(json.dumps({
                                "type": "error",
                                "message": str(e)
                            }) + '\n')
                            pipe_handle.flush()
                        except:
                            pass
                    completion_event.set()

            # Execute VLM completion
            # vlm_chat_completion_create executes asynchronously in the C++ layer.
            # We MUST wait here to prevent Python's garbage collector from destroying
            # the query and image_buffer objects while the C++ thread is still reading them!
            self.lib.vlm_chat_completion_create(self.vlm_handle, query, streaming, callback)

            # Wait for execution to finish (or timeout after 5 minutes)
            if not completion_event.wait(timeout=300.0):
                raise RuntimeError("VLM execution timed out after 300 seconds")

            if execution_error:
                raise execution_error

            logger.info(f"[{event_id}] VLM execution completed")

        except Exception as e:
            logger.error(f"[{event_id}] Error executing VLM: {e}", exc_info=True)
            self._send_error(event_id, str(e))
            if 'pipe_handle' in locals() and pipe_handle:
                try:
                    pipe_handle.write(json.dumps({
                        "type": "error",
                        "message": str(e)
                    }) + '\n')
                    pipe_handle.flush()
                except:
                    pass
        finally:
            if 'pipe_handle' in locals() and pipe_handle:
                try:
                    pipe_handle.close()
                    logger.info(f"[{event_id}] Closed output pipe")
                except Exception as e:
                    logger.warning(f"[{event_id}] Error closing pipe: {e}")

            # Send READY for next request over socket
            try:
                ready_response = InferenceProtocol.create_ready_response(event_id=event_id)
                self._send_response(ready_response)
                logger.info(f"[{event_id}] Sent READY response over socket")
            except Exception as e:
                logger.error(f"[{event_id}] Error sending READY response: {e}")

    def _handle_reset(self, command: dict):
        """Handle RESET command - (No-op for VLM as it is standalone)."""
        try:
            logger.info("VLM received RESET command, ignoring as VLM is standalone")

            # Send READY response
            response = InferenceProtocol.create_ready_response()
            self._send_response(response)
            logger.info("Sent READY response after ignoring reset")

        except Exception as e:
            logger.error(f"Error handling reset for VLM: {e}", exc_info=True)
            self._send_error(None, f"Reset failed: {e}")

    def _cleanup(self):
        """Cleanup resources."""
        logger.info("Cleaning up VLM process")

        try:
            # Destroy VLM handle
            if self.vlm_handle and self.lib:
                logger.info("Destroying VLM handle")
                self.lib.vlm_destroy_object(self.vlm_handle)
                self.vlm_handle = None
        except Exception as e:
            logger.error(f"Error destroying VLM handle: {e}")

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

        logger.info("VLM process cleanup complete")


def main():
    """Main entry point."""
    # Get socket FD from environment variable
    socket_fd_str = os.environ.get("VLM_SOCKET_FD")
    if not socket_fd_str:
        logger.error("VLM_SOCKET_FD environment variable not set")
        sys.exit(1)

    try:
        socket_fd = int(socket_fd_str)
    except ValueError:
        logger.error(f"Invalid VLM_SOCKET_FD value: {socket_fd_str}")
        sys.exit(1)

    logger.info("Starting VLM Process")
    logger.info(f"Socket FD: {socket_fd}")
    logger.info(f"PID: {os.getpid()}")

    process = VLMProcess(socket_fd=socket_fd)
    process.start()

    logger.info("VLM Process exiting")


if __name__ == "__main__":
    main()
