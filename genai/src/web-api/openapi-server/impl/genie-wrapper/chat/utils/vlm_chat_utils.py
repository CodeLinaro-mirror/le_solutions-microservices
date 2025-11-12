# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.error import Error
from openapi_server.impl.model_config_manager import ModelConfigManager
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.impl.genie_wrapper.utils.image_validator import extract_and_decode_images, has_image_content
from openapi_server.impl.genie_wrapper.utils.image_preprocessor import preprocess_from_decoded
from openapi_server.impl.constant import HttpStatusCodes
from openapi_server.impl.constant import ErrorMessages, Parameters, LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST
import logging
import uuid

LoggerConfig.initialize(level=logging.DEBUG)
logger = LoggerConfig.get_logger(__name__)

from fastapi import HTTPException


class VLMChatQueryUtils:
    @staticmethod
    def vlm_compose_query(request_data: CreateChatCompletionRequest, completion_id: str = None, raw_json: dict = None):
        """
        Compose a VLM query with multimodal content (text + images).

        Args:
            request_data: The chat completion request
            completion_id: Optional completion ID
            raw_json: Raw JSON request body to bypass Pydantic deserialization issues

        Returns:
            tuple: (vlm_service, model_str, config_file, query, completion_id, image_buffers)
        """
        if not request_data.messages:
            raise ValueError("No messages provided in request")

        last_msg = request_data.messages[-1]
        if not last_msg.content or (isinstance(last_msg.content, str) and not last_msg.content.strip()):
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)

        requested_model = getattr(request_data, 'model', None)

        if requested_model and requested_model.strip():
            logger.info(f"VLM Model requested from API: {requested_model}")
            config_manager = ModelConfigManager()

            if not config_manager.supports_vision(requested_model):
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=f"Model {requested_model} does not support vision/image inputs"
                )

            internal_model_id = config_manager.get_internal_id(requested_model)
            if not internal_model_id:
                internal_model_id = requested_model
                logger.warning(f"Could not map model {requested_model}, using as-is")

            model_str = internal_model_id
        else:
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail="Model must be specified for VLM requests"
            )

        logger.info(f"Using VLM model: {model_str}")

        # TEST MODE: Use known-working preprocessed file instead of our preprocessing
        USE_TEST_FILE = False
        TEST_FILE_PATH = "pixel_values.raw"

        all_pixel_bytes = b''

        if USE_TEST_FILE:
            # Check if request has images
            has_images = False
            if raw_json:
                raw_messages = raw_json.get('messages', [])
                has_images = has_image_content(raw_messages)
            else:
                has_images = has_image_content(request_data.messages)

            if has_images:
                logger.info(f"TEST MODE: Using known-working preprocessed file: {TEST_FILE_PATH}")
                try:
                    with open(TEST_FILE_PATH, 'rb') as f:
                        all_pixel_bytes = f.read()
                    logger.info(f"TEST MODE: Loaded preprocessed file successfully: {len(all_pixel_bytes)} bytes")
                    logger.info(f"TEST MODE: This matches the working C test application approach")
                except FileNotFoundError:
                    logger.error(f"TEST MODE: File not found: {TEST_FILE_PATH}")
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=f"Test preprocessed file not found: {TEST_FILE_PATH}"
                    )
                except Exception as e:
                    logger.error(f"TEST MODE: Failed to read {TEST_FILE_PATH}: {e}")
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=f"Failed to read test preprocessed file: {str(e)}"
                    )
            else:
                logger.warning("TEST MODE: No images detected in request, skipping test file")
        else:
            # Original preprocessing logic (currently disabled for testing)
            logger.info("Extracting and preprocessing images from messages")

            if raw_json:
                raw_messages = raw_json.get('messages', [])
                logger.info(f"FIX: Using raw JSON messages for image extraction (bypassing Pydantic OneOf)")
                decoded_images = extract_and_decode_images(raw_messages)
            else:
                logger.warning("No raw JSON provided - falling back to Pydantic models (may fail for images)")
                decoded_images = extract_and_decode_images(request_data.messages)

            logger.info(f"Found {len(decoded_images)} images to preprocess")

            preprocessed_images = []
            for i, decoded_img in enumerate(decoded_images):
                try:
                    logger.info(f"Preprocessing image {i+1}/{len(decoded_images)}: original {decoded_img.width}x{decoded_img.height}, format={decoded_img.format}")
                    logger.info(f"Image {i+1} will be downscaled to 512x342 RGB as per model requirements")
                    preprocessed = preprocess_from_decoded(decoded_img)
                    preprocessed_images.append(preprocessed)
                    logger.info(f"Image {i+1} preprocessed: {preprocessed.num_patches} patches, {preprocessed.patch_dim} dims per patch")
                    logger.info(f"Image {i+1} final dimensions: {preprocessed.resized_width}x{preprocessed.resized_height} (from original {preprocessed.original_width}x{preprocessed.original_height})")
                except Exception as e:
                    logger.error(f"Failed to preprocess image {i+1}: {e}")
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=f"Failed to preprocess image: {str(e)}"
                    )

            if len(preprocessed_images) == 1:
                logger.info("Only 1 image provided, duplicating for VLM backend requirement (minimum 2 images)")
                preprocessed_images.append(preprocessed_images[0])
                logger.info(f"Image duplicated. Total images for VLM: {len(preprocessed_images)}")

            for i, preprocessed in enumerate(preprocessed_images):
                pixel_bytes = preprocessed.to_bytes()
                all_pixel_bytes += pixel_bytes
                logger.info(f"Image {i+1}: {len(pixel_bytes)} bytes, {preprocessed.num_patches} patches, {preprocessed.patch_dim} dims per patch")
            logger.info(f"Total concatenated image buffer: {len(all_pixel_bytes)} bytes from {len(preprocessed_images)} images")

        # VLM Service and handle management (not used in the handler-based flow, but output API is unchanged)
        vlm_service = None  # Placeholder for compatibility
        config_manager = ModelConfigManager()
        config_file = config_manager.get_config_file_path(requested_model)
        if not config_file:
            logger.warning(f"No config file found for model {requested_model}, using empty string")
            config_file = ""
        else:
            logger.info(f"Using config file for VLM: {config_file}")

        openai_uuid = f"chatcmpl-{uuid.uuid4().hex[:24]}"
        completion_id = openai_uuid

        # Compose a "query" object (mock or leave blank for new handler flow)
        query = None
        image_buffers = []
        if len(all_pixel_bytes) > 0:
            image_buffers = [all_pixel_bytes]

            if USE_TEST_FILE:
                logger.info(f"TEST MODE: Added image buffer from test file: {len(all_pixel_bytes)} bytes")
            else:
                logger.info(f"Added image buffers: {len(all_pixel_bytes)} bytes total")
        else:
            logger.warning("No image buffer to add - request may fail if model expects images")
            image_buffers = []

        if USE_TEST_FILE:
            logger.info(f"TEST MODE: VLM query composed successfully using test file")
        else:
            logger.info(f"VLM query composed successfully")

        # New interface: return placeholder vlm_service, model_str, config_file, query, completion_id, image_buffers
        return vlm_service, model_str, config_file, query, completion_id, image_buffers
