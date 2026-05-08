# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from datetime import datetime
from fastapi import HTTPException
import io
import base64
import os
import re
from PIL import Image
import numpy as np

from openapi_server.apis.image_api_base import BaseImageApi
from openapi_server.models.image_generation_request import ImageGenerationRequest
from openapi_server.models.image_response import ImageResponse
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.interface.stable_diffusion_executor import StableDiffusionExecutor
from openapi_server.managers.model_config_manager import model_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ImageApiImpl(BaseImageApi):
    async def v1_images_generations_post(self, request: ImageGenerationRequest) -> ImageResponse:
        """
        Generate image from text prompt using StableDiffusionExecutor.
        """
        try:
            logger.debug(f"Received image generation request: {request}")

            #Validate prompt
            if not request.prompt or request.prompt.strip() == "":
                logger.error("Prompt is missing or empty.")
                raise HTTPException(status_code = HttpStatusCodes.BAD_REQUEST,
                                    detail = ErrorMessages.INCORRECT_CONTENT)

            if not re.search(r'[a-zA-Z]', request.prompt):
                logger.error(f"Prompt contains no alphabetic characters. Received: {request.prompt}")
                raise HTTPException(status_code = HttpStatusCodes.BAD_REQUEST,
                                    detail = ErrorMessages.INVALID_PROMPT)

            # Default model is the first model listed in the 'models' section of models_config.json
            default_model = model_config_manager.get_default_model()
            
            # Determine which model to use
            requested_model = request.model or default_model
            
            # Validate that the requested model exists in config
            if not model_config_manager.is_model_available(requested_model):
                logger.error(f"Requested model '{requested_model}' not found in configuration.")
                available_model_ids = model_config_manager.get_available_model_ids()
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=f"Model '{requested_model}' is not available. Available models: {available_model_ids}"
                )
            
            # Extract variant from model ID using the config manager
            variant = model_config_manager.extract_variant_from_model_id(requested_model)
            
            logger.info(f"Using model: {requested_model}, variant: {variant}")

            if request.quality == "low":
                steps_for_model = 20
            elif request.quality == "medium":
                steps_for_model = 50
            elif request.quality == "high":
                steps_for_model = 100

            #Fetch environment variable for models_path
            models_path = os.getenv("MODELS_PATH", "/opt/image_gen")

            # Get model file names from config manager
            model_info = model_config_manager.get_model_info(requested_model)
            model_files = model_info.get("model_files", {}) if model_info else {}

            # Validate that all required model file keys are present in the config
            required_model_file_keys = ["text_encoder", "unet", "vae"]
            missing_keys = [k for k in required_model_file_keys if k not in model_files]
            if missing_keys:
                logger.error(
                    f"Model '{requested_model}' configuration is missing required "
                    f"model_files keys: {missing_keys}"
                )
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=(
                        f"Model '{requested_model}' configuration is missing required "
                        f"model_files keys: {missing_keys}. Please check the models config file."
                    )
                )

            #Prepare config for StableDiffusionExecutor
            config = {
                "prompt": request.prompt,
                "seed": 0,
                "steps": steps_for_model,
                "guidance_scale": 7.5,
                "models_path": models_path,
                "variant": variant,
                "text_encoder_model": model_files["text_encoder"],
                "unet_model": model_files["unet"],
                "vae_model": model_files["vae"]
            }

            #Initialize executor and handle errors
            try:
                executor = StableDiffusionExecutor(config)
            except ValueError or TypeError as ve:
                logger.error(f"Configuration error: {ve}")
                raise HTTPException(status_code = HttpStatusCodes.BAD_REQUEST, detail=str(ve))
            except Exception as e:
                logger.error(f"Executor initialization failed: {e}")
                raise HTTPException(status_code = HttpStatusCodes.INTERNAL_SERVER_ERROR,
                                    detail = ErrorMessages.UNEXPECTED_ERROR)

            #Generate image and handle errors
            try:
                raw_image = executor.generate_image()  # Expected shape (1, 512, 512, 3)
            except Exception as e:
                logger.error(f"Image generation failed: {e}")
                raise HTTPException(status_code = HttpStatusCodes.INTERNAL_SERVER_ERROR,
                                    detail = ErrorMessages.UNEXPECTED_ERROR)

            if raw_image is None or not isinstance(raw_image, np.ndarray):
                logger.error("StableDiffusionExecutor returned invalid image data.")
                raise HTTPException(status_code = HttpStatusCodes.INTERNAL_SERVER_ERROR,
                                    detail = ErrorMessages.UNEXPECTED_ERROR)

            #Convert raw image to uint8 and PIL
            raw_image = (raw_image * 255.0).clip(0, 255).astype(np.uint8)[0]
            pil_img = Image.fromarray(raw_image, mode="RGB")

            #Determine output format
            image_format = request.output_format or "png"
            img_bytes = io.BytesIO()
            pil_img.save(img_bytes, format=image_format.upper())
            img_bytes.seek(0)

            #Encode to base64
            b64_json = base64.b64encode(img_bytes.read()).decode("utf-8")

            #Prepare response data
            response_data = [{
                "url": None,
                "b64_json": b64_json
            }]

            response = ImageResponse(
                created = int(datetime.now().timestamp()),
                data = response_data,
                size = request.size or "512 x 512",
                output_format = image_format,
                quality = request.quality
            )

            logger.info("Image generation response prepared successfully.")
            return response

        except HTTPException:
            raise
        except Exception as e:
            logger.error(f"Unexpected error in generate_image: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                                detail=ErrorMessages.UNEXPECTED_ERROR)
