# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from datetime import datetime
from fastapi import HTTPException
import io
import base64
import re
from PIL import Image
import numpy as np

from openapi_server.apis.image_api_base import BaseImageApi
from openapi_server.models.image_generation_request import ImageGenerationRequest
from openapi_server.models.image_response import ImageResponse
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.interface.image_generator_factory import image_generator_factory
from openapi_server.managers.model_config_manager import model_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ImageApiImpl(BaseImageApi):
    async def v1_images_generations_post(self, request: ImageGenerationRequest) -> ImageResponse:
        """
        Generate image from text prompt using the ImageGeneratorFactory.

        The factory returns a cached executor instance for the requested model,
        creating and initialising it (loading QNN context binaries, starting the
        QNN runtime) only on the first request for that model.  Subsequent
        requests reuse the same instance, avoiding the expensive initialisation
        overhead on every call.
        """
        try:
            logger.debug(f"Received image generation request: {request}")

            # Validate prompt
            if not request.prompt or request.prompt.strip() == "":
                logger.error("Prompt is missing or empty.")
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=ErrorMessages.INCORRECT_CONTENT,
                )

            if not re.search(r"[a-zA-Z]", request.prompt):
                logger.error(
                    f"Prompt contains no alphabetic characters. Received: {request.prompt}"
                )
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=ErrorMessages.INVALID_PROMPT,
                )

            # Default model is the first model listed in models_config.json
            default_model = model_config_manager.get_default_model()
            requested_model = request.model or default_model

            # Validate that the requested model exists in config
            if not model_config_manager.is_model_available(requested_model):
                logger.error(
                    f"Requested model '{requested_model}' not found in configuration."
                )
                available_model_ids = model_config_manager.get_available_model_ids()
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=(
                        f"Model '{requested_model}' is not available. "
                        f"Available models: {available_model_ids}"
                    ),
                )

            # Extract variant from model ID (e.g. "stable-diffusion-2.1" → "2.1")
            variant = model_config_manager.extract_variant_from_model_id(requested_model)
            logger.info(f"Using model: {requested_model}, variant: {variant}")

            # Map quality level to inference step count
            QUALITY_STEPS = {"low": 20, "medium": 50, "high": 100}
            steps_for_model = QUALITY_STEPS.get(request.quality, 20)

            # Wire the style field to prompt modifiers
            STYLE_MODIFIERS = {
                "natural": (
                    "photorealistic, hyperrealistic, RAW photo, sharp focus, "
                    "natural lighting, ultra-detailed, 8K, DSLR, cinematic"
                ),
                "vivid": (
                    "vivid colors, highly detailed, digital art, concept art, "
                    "dramatic lighting, artstation, trending, vibrant"
                ),
            }
            style_modifier = STYLE_MODIFIERS.get(request.style, "")
            effective_prompt = (
                f"{request.prompt}, {style_modifier}" if style_modifier else request.prompt
            )
            logger.info(
                f"Style '{request.style}' applied. Effective prompt: '{effective_prompt}'"
            )

            # Get model info from the discovery manager.
            # model_info["bundle_dir"] is the specific aihub bundle directory
            # (e.g. /mnt/work/models/stable_diffusion_v2_1-qnn_context_binary-...)
            # which contains both metadata.json and the QNN binary files.
            model_info = model_config_manager.get_model_info(requested_model)
            if model_info is None:
                logger.error(f"Model info not found for '{requested_model}'.")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=ErrorMessages.UNEXPECTED_ERROR,
                )

            model_files = model_info.get("model_files", {})
            bundle_dir = model_info.get("bundle_dir", "")

            # Validate that all required model file keys are present
            required_model_file_keys = ["text_encoder", "unet", "vae"]
            missing_keys = [k for k in required_model_file_keys if k not in model_files]
            if missing_keys:
                logger.error(
                    f"Model '{requested_model}' bundle is missing components: {missing_keys}"
                )
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=(
                        f"Model '{requested_model}' bundle is missing components: "
                        f"{missing_keys}. Check the bundle's metadata.json."
                    ),
                )

            # Infrastructure config – bundle directory and file names only.
            # Per-request parameters (prompt, seed, steps, guidance_scale) are
            # passed directly to generate_image() so the cached executor can
            # serve many different requests without being re-created.
            infra_config = {
                "models_path": bundle_dir,
                "text_encoder_model": model_files["text_encoder"],
                "unet_model": model_files["unet"],
                "vae_model": model_files["vae"],
            }

            # Obtain (or create) the executor for this model via the factory
            try:
                executor = image_generator_factory.get_or_create(
                    model_id=requested_model,
                    variant=variant,
                    config=infra_config,
                )
            except (ValueError, TypeError) as ve:
                logger.error(f"Executor configuration error: {ve}")
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST, detail=str(ve)
                )
            except Exception as e:
                logger.error(f"Executor initialisation failed: {e}")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=ErrorMessages.UNEXPECTED_ERROR,
                )

            # Generate image – per-request parameters passed here
            try:
                raw_image = executor.generate_image(
                    prompt=effective_prompt,
                    seed=0,
                    steps=steps_for_model,
                    guidance_scale=9.0,
                )
            except Exception as e:
                logger.error(f"Image generation failed: {e}")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=ErrorMessages.UNEXPECTED_ERROR,
                )

            if raw_image is None or not isinstance(raw_image, np.ndarray):
                logger.error("Executor returned invalid image data.")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=ErrorMessages.UNEXPECTED_ERROR,
                )

            # Convert raw float32 image to uint8 PIL image
            raw_image = (raw_image * 255.0).clip(0, 255).astype(np.uint8)[0]
            pil_img = Image.fromarray(raw_image, mode="RGB")

            # Determine output format
            image_format = request.output_format or "png"
            img_bytes = io.BytesIO()
            pil_img.save(img_bytes, format=image_format.upper())
            img_bytes.seek(0)

            # Encode to base64
            b64_json = base64.b64encode(img_bytes.read()).decode("utf-8")

            response = ImageResponse(
                created=int(datetime.now().timestamp()),
                data=[{"url": None, "b64_json": b64_json}],
                size=request.size or "512 x 512",
                output_format=image_format,
                quality=request.quality,
            )

            logger.info("Image generation response prepared successfully.")
            return response

        except HTTPException:
            raise
        except Exception as e:
            logger.error(f"Unexpected error in generate_image: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.UNEXPECTED_ERROR,
            )
