#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Standalone VLM test script that calls the CFFI VLM directly.

This script:
1. Downloads an image from a URL
2. Preprocesses the image using the same pipeline as the web API
3. Creates a VLM object via CFFI
4. Builds a query with text and image content
5. Calls vlm_chat_completion_create
6. Prints the response

Usage:
    python vlm_standalone_test.py [options]

Options:
    --model MODEL             Model ID (default: qwen2-vl-3b)
    --image-url URL          Image URL to download and process
    --prompt TEXT            Text prompt (default: "What is in this image?")
    --max-tokens N           Maximum tokens to generate (default: 300)
    --temperature FLOAT      Temperature (default: 0.7)
    --top-p FLOAT            Top-p sampling (default: 0.9)
    --presence-penalty FLOAT Presence penalty (default: 0.0)
    --frequency-penalty FLOAT Frequency penalty (default: 0.0)
    --stream                 Enable streaming mode (default: False)
    --config-path PATH       Path to models_config.json (optional)

Environment variables:
    VLM_LIBRARY_PATH         Path to libvlmservice.so (required)
    GENAI_INTERFACE_FILE     Path to genai_interface.h (required)
    GENAI_MODELS_CONFIG_PATH Path to models_config.json (optional)

Example:
    python vlm_standalone_test.py --model qwen2-vl-3b --image-url "https://example.com/image.jpg"

Container usage:
    docker run --rm --network=host vlm-service:latest python /usr/src/app/web-api/openapi_server/impl/genie_wrapper/vlm_standalone_test.py --model qwen2-vl-3b --image-url "https://example.com/image.jpg"
"""

import os
import sys
import argparse
import json
import logging
import threading
import time
from typing import Dict, Any, Optional, List, Tuple
import queue

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger("vlm_standalone_test")

# Try to import from openapi_server
# If that fails, adjust sys.path to include the repo path
try:
    from openapi_server.utils.image_validator import decode_image
    from openapi_server.utils.image_preprocessor import preprocess_from_decoded
    from openapi_server.managers.model_config_manager import ModelConfigManager
    from openapi_server.utils.common_utils import CommonUtils
except ImportError:
    logger.info("Failed to import directly, adjusting sys.path...")
    # Add the repo path to sys.path
    repo_path = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../../'))
    sys.path.insert(0, repo_path)

    try:
        from openapi_server.utils.image_validator import decode_image
        from openapi_server.utils.image_preprocessor import preprocess_from_decoded
        from openapi_server.managers.model_config_manager import ModelConfigManager
        from openapi_server.utils.common_utils import CommonUtils
    except ImportError as e:
        logger.error(f"Failed to import required modules: {e}")
        logger.error("Make sure you're running from the correct directory or inside the container")
        sys.exit(1)

# Import CFFI
try:
    from cffi import FFI
except ImportError:
    logger.error("CFFI not found. Install with: pip install cffi")
    sys.exit(1)

def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Standalone VLM test script")
    parser.add_argument("--model", default="qwen2-vl-3b", help="Model ID (default: qwen2-vl-3b)")
    parser.add_argument("--image-url", help="Image URL to download and process")
    parser.add_argument("--image-base64", help="Base64 encoded image (with or without data URL prefix)")
    parser.add_argument("--image-file", help="Path to file containing preprocessed image bytes")
    parser.add_argument("--prompt", default="What is in this image?", help="Text prompt")
    parser.add_argument("--max-tokens", type=int, default=300, help="Maximum tokens to generate")
    parser.add_argument("--temperature", type=float, default=0.7, help="Temperature")
    parser.add_argument("--top-p", type=float, default=0.9, help="Top-p sampling")
    parser.add_argument("--presence-penalty", type=float, default=0.0, help="Presence penalty")
    parser.add_argument("--frequency-penalty", type=float, default=0.0, help="Frequency penalty")
    parser.add_argument("--stream", action="store_true", help="Enable streaming mode")
    parser.add_argument("--config-path", help="Path to models_config.json")
    parser.add_argument("--output-pipe", help="Named pipe path for structured output")

    return parser.parse_args()

def get_header_content() -> str:
    """Get the content of the genai_interface.h header file."""
    header_path = os.environ.get("GENAI_INTERFACE_FILE")
    if not header_path or not os.path.exists(header_path):
        # Try to find it in the repo
        repo_header_path = os.path.join(
            os.path.dirname(__file__),
            "include",
            "genai_interface.h"
        )
        if os.path.exists(repo_header_path):
            header_path = repo_header_path
        else:
            logger.error("GENAI_INTERFACE_FILE environment variable not set or file not found")
            logger.error("Please set GENAI_INTERFACE_FILE to the path of genai_interface.h")
            sys.exit(1)

    logger.info(f"Using header file: {header_path}")
    with open(header_path, 'r') as f:
        return f.read()

def get_library_path() -> str:
    """Get the path to the libvlmservice.so library."""
    lib_path = os.environ.get("VLM_LIBRARY_PATH")
    if not lib_path or not os.path.exists(lib_path):
        logger.error("VLM_LIBRARY_PATH environment variable not set or file not found")
        logger.error("Please set VLM_LIBRARY_PATH to the path of libvlmservice.so")
        sys.exit(1)

    logger.info(f"Using library: {lib_path}")
    return lib_path

def resolve_model_config(model_id: str, config_path: Optional[str] = None) -> Tuple[str, str]:
    """
    Resolve the model ID and config file path.

    Args:
        model_id: External model ID (e.g., "qwen2-vl-3b")
        config_path: Optional path to models_config.json

    Returns:
        Tuple of (internal_model_id, config_file_path)
    """
    # Set GENAI_MODELS_CONFIG_PATH if provided
    if config_path:
        os.environ["GENAI_MODELS_CONFIG_PATH"] = config_path

    try:
        config_manager = ModelConfigManager()

        # Check if model exists
        if not config_manager.validate_model(model_id):
            logger.warning(f"Model {model_id} not found in config, using as-is")
            return model_id, ""

        # Get internal ID
        internal_id = config_manager.get_internal_id(model_id)
        if not internal_id:
            logger.warning(f"No internal ID found for {model_id}, using as-is")
            internal_id = model_id

        # Get config file
        config_file = config_manager.get_config_file_path(model_id)
        if not config_file:
            logger.warning(f"No config file found for {model_id}, using empty string")
            config_file = ""

        logger.info(f"Resolved model {model_id} to internal ID {internal_id} with config {config_file}")
        return internal_id, config_file

    except Exception as e:
        logger.error(f"Error resolving model config: {e}")
        logger.warning(f"Falling back to using model ID as-is: {model_id}")
        return model_id, ""

def decode_base64_image(image_base64: str) -> bytes:
    """
    Decode and preprocess a base64 encoded image.

    Args:
        image_base64: Base64 encoded image (with or without data URL prefix)

    Returns:
        Preprocessed image as bytes
    """
    logger.info("Processing base64 encoded image")

    try:
        import base64
        from PIL import Image
        import io

        # Handle both formats:
        # 1. data:image/jpeg;base64,<data>
        # 2. <raw base64 data>
        if image_base64.startswith('data:image'):
            # Extract base64 part after comma
            if ',' in image_base64:
                base64_data = image_base64.split(',', 1)[1]
                logger.info("Extracted base64 data from data URL format")
            else:
                logger.error("Invalid data URL format: missing comma separator")
                raise ValueError("Invalid data URL format")
        else:
            # Assume raw base64 data
            base64_data = image_base64
            logger.info("Processing raw base64 data")

        # Decode base64
        try:
            image_data = base64.b64decode(base64_data)
            logger.info(f"Base64 decoded: {len(image_data)} bytes")
        except Exception as e:
            logger.error(f"Failed to decode base64 data: {e}")
            raise ValueError(f"Invalid base64 data: {e}")

        # Create PIL Image from bytes
        try:
            img = Image.open(io.BytesIO(image_data))
            logger.info(f"PIL Image created: {img.width}x{img.height}, format={img.format}, mode={img.mode}")
        except Exception as e:
            logger.error(f"Failed to create PIL Image from decoded data: {e}")
            raise ValueError(f"Invalid image data: {e}")

        # Preprocess the image using existing pipeline
        from openapi_server.utils.image_preprocessor import preprocess_image
        preprocessed = preprocess_image(img)
        logger.info(f"Image preprocessed: {preprocessed.num_patches} patches, {preprocessed.patch_dim} dims per patch")
        logger.info(f"Image dimensions: {preprocessed.resized_width}x{preprocessed.resized_height} (from original {preprocessed.original_width}x{preprocessed.original_height})")

        # Convert to bytes
        pixel_bytes = preprocessed.to_bytes()
        logger.info(f"Image converted to bytes: {len(pixel_bytes)} bytes")

        return pixel_bytes

    except Exception as e:
        logger.error(f"Error processing base64 image: {e}")
        raise


def download_and_preprocess_image(image_url: str) -> bytes:
    """
    Download and preprocess an image from a URL.

    Args:
        image_url: URL of the image to download

    Returns:
        Preprocessed image as bytes
    """
    logger.info(f"Downloading and preprocessing image from: {image_url}")

    try:
        # Download and decode the image
        decoded_img = decode_image(image_url)
        logger.info(f"Image downloaded and decoded: {decoded_img.width}x{decoded_img.height}, format={decoded_img.format}")

        # Preprocess the image
        preprocessed = preprocess_from_decoded(decoded_img)
        logger.info(f"Image preprocessed: {preprocessed.num_patches} patches, {preprocessed.patch_dim} dims per patch")
        logger.info(f"Image dimensions: {preprocessed.resized_width}x{preprocessed.resized_height} (from original {preprocessed.original_width}x{preprocessed.original_height})")

        # Convert to bytes
        pixel_bytes = preprocessed.to_bytes()
        logger.info(f"Image converted to bytes: {len(pixel_bytes)} bytes")

        return pixel_bytes

    except Exception as e:
        logger.error(f"Error preprocessing image: {e}")
        raise


def get_image_bytes(args) -> bytes:
    """
    Get preprocessed image bytes from URL, base64, or file.

    Args:
        args: Parsed command line arguments

    Returns:
        Preprocessed image bytes

    Raises:
        ValueError: If no image source is provided or multiple sources are provided
    """
    # Count how many image sources are provided
    sources = [args.image_url, args.image_base64, args.image_file]
    provided_sources = [s for s in sources if s is not None]

    if len(provided_sources) == 0:
        raise ValueError("One of --image-url, --image-base64, or --image-file is required")
    elif len(provided_sources) > 1:
        raise ValueError("Please provide only one of --image-url, --image-base64, or --image-file")

    if args.image_file:
        # Read preprocessed image bytes directly from file
        logger.info(f"Reading preprocessed image bytes from file: {args.image_file}")
        try:
            with open(args.image_file, 'rb') as f:
                image_bytes = f.read()
            logger.info(f"Read {len(image_bytes)} bytes from file")
            return image_bytes
        except Exception as e:
            logger.error(f"Failed to read image file: {e}")
            raise ValueError(f"Invalid image file: {e}")
    elif args.image_base64:
        return decode_base64_image(args.image_base64)
    elif args.image_url:
        return download_and_preprocess_image(args.image_url)

class VLMExecutor:
    """
    Class to execute VLM operations in a dedicated thread.
    This mimics the VLMExecutionThread in the web API.
    """
    def __init__(self, ffi: FFI, lib):
        self.ffi = ffi
        self.lib = lib
        self.thread = None
        self.request_queue = queue.Queue()
        self.running = False
        self.completion_event = threading.Event()
        self.result = None
        self.error = None

    def start(self):
        """Start the executor thread."""
        if self.thread is not None and self.thread.is_alive():
            logger.warning("VLM executor thread already running")
            return

        self.running = True
        self.thread = threading.Thread(target=self._run, name="VLMExecutorThread")
        self.thread.daemon = True
        self.thread.start()
        logger.info("VLM executor thread started")

    def _run(self):
        """Main thread loop."""
        logger.info("VLM executor thread running")

        while self.running:
            try:
                request = self.request_queue.get(timeout=1.0)
                request_type = request.get('type')

                if request_type == 'execute':
                    self._handle_execute(request)
                elif request_type == 'shutdown':
                    logger.info("Received shutdown request")
                    break
                else:
                    logger.error(f"Unknown request type: {request_type}")

            except queue.Empty:
                continue
            except Exception as e:
                logger.error(f"Error in VLM executor thread: {e}")
                self.error = str(e)
                self.completion_event.set()

        logger.info("VLM executor thread exiting")

    def _handle_execute(self, request):
        """Handle an execute request."""
        try:
            model_str = request['model_str']
            config_file = request['config_file']
            query = request['query']
            streaming = request['streaming']
            callback = request['callback']

            # Create VLM object
            logger.info(f"Creating VLM object with model={model_str}, config={config_file}")
            model_input = self.ffi.new("char[]", model_str.encode('utf-8'))
            config_path = self.ffi.new("char[]", config_file.encode('utf-8'))

            handle = self.lib.vlm_create_object(model_input, config_path, streaming)

            if handle == self.ffi.NULL:
                error_msg = f"Failed to create VLM handle for model: {model_str}"
                logger.error(error_msg)
                self.error = error_msg
                self.completion_event.set()
                return

            logger.info("VLM handle created successfully")

            try:
                # Execute VLM completion
                logger.info(f"Executing VLM completion with streaming={streaming}")
                self.lib.vlm_chat_completion_create(handle, query, streaming, callback)
                logger.info("VLM completion executed successfully")
            finally:
                # Always destroy the handle
                logger.info("Destroying VLM handle")
                self.lib.vlm_destroy_object(handle)

        except Exception as e:
            logger.error(f"Error executing VLM: {e}")
            self.error = str(e)
            self.completion_event.set()

    def submit_execute(self, model_str, config_file, query, streaming, callback):
        """Submit an execute request."""
        self.start()  # Ensure thread is running

        # Reset completion event and result
        self.completion_event.clear()
        self.result = None
        self.error = None

        # Create and submit request
        request = {
            'type': 'execute',
            'model_str': model_str,
            'config_file': config_file,
            'query': query,
            'streaming': streaming,
            'callback': callback
        }

        logger.info(f"Submitting execute request for model={model_str}, streaming={streaming}")
        self.request_queue.put(request)

    def wait_for_completion(self, timeout=300):
        """Wait for completion and return the result."""
        if not self.completion_event.wait(timeout):
            raise TimeoutError("Timeout waiting for VLM completion")

        if self.error:
            raise RuntimeError(f"VLM execution failed: {self.error}")

        return self.result

    def shutdown(self):
        """Shutdown the executor thread."""
        if self.thread is None or not self.thread.is_alive():
            return

        logger.info("Shutting down VLM executor thread")
        self.running = False
        self.request_queue.put({'type': 'shutdown'})
        self.thread.join(timeout=10)

        if self.thread.is_alive():
            logger.warning("VLM executor thread did not exit cleanly")

def main():
    """Main function."""
    args = parse_args()

    # Resolve model and config
    model_str, config_file = resolve_model_config(args.model, args.config_path)

    # Get preprocessed image bytes from URL or base64
    try:
        image_bytes = get_image_bytes(args)
    except Exception as e:
        logger.error(f"Failed to process image: {e}")
        sys.exit(1)

    # Initialize CFFI
    ffi = FFI()
    header_content = get_header_content()
    ffi.cdef(header_content)

    try:
        lib = ffi.dlopen(get_library_path())
    except Exception as e:
        logger.error(f"Failed to load VLM library: {e}")
        sys.exit(1)

    # Create VLM executor
    executor = VLMExecutor(ffi, lib)

    # Create query structure
    query = ffi.new("Query *")
    if query == ffi.NULL:
        logger.error("Failed to allocate Query pointer")
        sys.exit(1)

    # Populate query fields
    CommonUtils.copy_py_string_to_c_array(ffi, query.model, model_str, 256)
    CommonUtils.copy_py_string_to_c_array(ffi, query.message.role, "user", 256)

    # Set multimodal mode
    query.message.use_content_items = True
    query.message.content_items_count = 0

    # Add text content item
    query.message.content_items[0].type = 0  # CONTENT_TYPE_TEXT
    CommonUtils.copy_py_string_to_c_array(
        ffi,
        query.message.content_items[0].text,
        args.prompt,
        12300  # MAX_CONTENT_LENGTH
    )
    query.message.content_items_count = 1
    logger.info(f"Added text content item: {args.prompt}")

    # Add image buffer
    buffer = ffi.new("char[]", len(image_bytes))
    ffi.memmove(buffer, image_bytes, len(image_bytes))

    idx = query.message.content_items_count
    query.message.content_items[idx].type = 1  # CONTENT_TYPE_IMAGE_BUFFER
    query.message.content_items[idx].image.buffer = buffer
    query.message.content_items[idx].image.size = len(image_bytes)
    query.message.content_items_count += 1
    logger.info(f"Added image content item: {len(image_bytes)} bytes")

    # Set numeric parameters
    CommonUtils.copy_py_int_to_c_field(ffi, query, 'max_completion_tokens', args.max_tokens)
    CommonUtils.copy_py_float_to_c_field(ffi, query, 'temperature', args.temperature)
    CommonUtils.copy_py_float_to_c_field(ffi, query, 'top_p', args.top_p)
    CommonUtils.copy_py_float_to_c_field(ffi, query, 'presence_penalty', args.presence_penalty)
    CommonUtils.copy_py_float_to_c_field(ffi, query, 'frequency_penalty', args.frequency_penalty)

    # Open output pipe if provided
    pipe_handle = None
    if args.output_pipe:
        try:
            pipe_handle = open(args.output_pipe, 'w', buffering=1)  # Line buffered
            logger.info(f"Opened output pipe: {args.output_pipe}")
        except Exception as e:
            logger.error(f"Failed to open output pipe: {e}")
            sys.exit(1)

    # Define callback
    streaming_output = []

    @ffi.callback("void(const Response *)")
    def callback(response_ptr):
        resp = response_ptr[0]
        choice = resp.choices[0]
        msg = choice.message

        content = ffi.string(msg.content).decode("utf-8")
        finish_reason = ffi.string(choice.finish_reason).decode("utf-8")

        # Write to pipe if provided
        if pipe_handle:
            try:
                # Send token
                pipe_handle.write(json.dumps({
                    "type": "token",
                    "content": content
                }) + '\n')
                pipe_handle.flush()

                # Send done if finished
                if finish_reason == "stop":
                    pipe_handle.write(json.dumps({
                        "type": "done",
                        "finish_reason": finish_reason
                    }) + '\n')
                    pipe_handle.flush()
            except Exception as e:
                logger.error(f"Error writing to pipe: {e}")

        if args.stream:
            # Print streaming output
            print(content, end="", flush=True)
            streaming_output.append(content)

        # Store result
        executor.result = {
            'id': ffi.string(resp.id).decode("utf-8"),
            'object': 'chat.completion',
            'created': resp.created,
            'model': ffi.string(resp.model).decode("utf-8"),
            'choices': [{
                'message': {
                    'role': ffi.string(msg.role).decode("utf-8"),
                    'content': content
                },
                'finish_reason': finish_reason,
                'index': 0
            }]
        }

        # Signal completion if not streaming or if finished
        if not args.stream or finish_reason == "stop":
            executor.completion_event.set()

    # Execute VLM completion
    try:
        executor.submit_execute(model_str, config_file, query, args.stream, callback)

        # Wait for completion
        result = executor.wait_for_completion()

        # Log result with uniform markers (only if not using pipe)
        if not pipe_handle:
            if not args.stream:
                logger.info("=== VLM_RESPONSE_START ===")
                logger.info(result['choices'][0]['message']['content'])
                logger.info("=== VLM_RESPONSE_END ===")
            else:
                # For streaming mode, log the accumulated output
                if streaming_output:
                    logger.info("=== VLM_RESPONSE_START ===")
                    logger.info(''.join(streaming_output))
                    logger.info("=== VLM_RESPONSE_END ===")

        logger.info(f"Finish reason: {result['choices'][0]['finish_reason']}")

    except Exception as e:
        logger.error(f"Error: {e}")
        # Write error to pipe if available
        if pipe_handle:
            try:
                pipe_handle.write(json.dumps({
                    "type": "error",
                    "message": str(e)
                }) + '\n')
                pipe_handle.flush()
            except:
                pass
        sys.exit(1)
    finally:
        # Close pipe if opened
        if pipe_handle:
            try:
                pipe_handle.close()
                logger.info("Closed output pipe")
            except Exception as e:
                logger.warning(f"Error closing pipe: {e}")

        # Shutdown executor
        executor.shutdown()

if __name__ == "__main__":
    main()
