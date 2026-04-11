# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import Dict, Callable, List
import asyncio
import os
import sys
import importlib.util
from common.base_service import BaseService
from common.redis_client import RedisClient
from common.config import Config
from common.logger import get_logger
from common.model_loader import ModelLoader
from common.service_coordinator import get_service_coordinator
from models.messages import (
    TranslationRequest,
    TranslationResponse,
    TranslationResult,
    TranslationModelsRequest,
    TranslationModelsResponse,
    get_message_type
)

logger = get_logger(__name__)


class T2TService(BaseService):
    """
    Text-to-Text Translation Service.
    Handles translation requests between different languages using a singleton
    TranslationWrapper instance — the same pattern as ASR (WhisperWrapper) and
    TTS (TTS wrapper).
    """

    def __init__(self, redis_client: RedisClient):
        super().__init__(redis_client, "T2T")
        self.dev_mode = Config.DEV_MODE
        self.model_config = ModelLoader.get_translation_models(self.dev_mode)
        self.available_models = []  # Populated during initialize()

        # Translation engine module (loaded dynamically from engine/python)
        self.t2t_engine = None

        # Singleton wrapper instance — one wrapper at a time, reused across requests.
        # Reinitialised only when the model/language pair changes.
        self.t2t_wrapper = None
        self.t2t_wrapper_lock = asyncio.Lock()
        self.current_wrapper_key = None  # "<model>_<input_lang>_<output_lang>"

        # Service coordinator for managing resource conflicts with ASR/TTS
        self.coordinator = get_service_coordinator()

        if not self.dev_mode:
            self.logger.info("Running in production mode - loading translation wrapper")
            try:
                # Try multiple possible locations for the translation wrapper
                possible_paths = [
                    os.path.join(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "translate_wrapper.py"
                    ),
                    "/usr/src/server/translate_wrapper.py",
                    os.path.join(os.path.dirname(__file__), "..", "..", "translate_wrapper.py")
                ]
                
                self.logger.info(f"Searching for translation wrapper in {len(possible_paths)} locations...")
                wrapper_path = None
                for path in possible_paths:
                    self.logger.info(f"  Checking: {path}")
                    if os.path.exists(path):
                        wrapper_path = path
                        self.logger.info(f"  ✓ Found translation wrapper at {path}")
                        break
                    else:
                        self.logger.info(f"  ✗ Not found at {path}")
                
                if wrapper_path:
                    self.logger.info(f"Loading translation wrapper from {wrapper_path}")
                    wrapper_dir = os.path.dirname(wrapper_path)
                    if wrapper_dir not in sys.path:
                        sys.path.insert(0, wrapper_dir)
                        self.logger.info(f"Added {wrapper_dir} to Python path")

                    spec = importlib.util.spec_from_file_location("translate_wrapper", wrapper_path)
                    if spec and spec.loader:
                        self.logger.info("Creating module from spec...")
                        self.t2t_engine = importlib.util.module_from_spec(spec)
                        sys.modules["translate_wrapper"] = self.t2t_engine
                        self.logger.info("Executing module...")
                        spec.loader.exec_module(self.t2t_engine)
                        self.logger.info("Module executed successfully")

                        if hasattr(self.t2t_engine, 'TranslationWrapper'):
                            self.logger.info("✓ TranslationWrapper class found")
                            # Try to instantiate to check if .so files are accessible
                            try:
                                # Note: TranslationWrapper requires model_path/model_dir, input_lang, output_lang
                                # We can't fully test it here without a model, but we can check if the class is callable
                                self.logger.info("✓ TranslationWrapper class is accessible (will test .so files on first use)")
                            except Exception as inst_error:
                                self.logger.error(f"✗ TranslationWrapper check failed: {inst_error}", exc_info=True)
                        else:
                            self.logger.error("✗ TranslationWrapper class not found in wrapper module")
                            self.t2t_engine = None
                    else:
                        self.logger.error("✗ Failed to load translation wrapper module specification")
                else:
                    self.logger.error(f"✗ Translation wrapper not found in any of the {len(possible_paths)} locations")
            except Exception as e:
                self.logger.error(f"✗ Error loading translation wrapper: {e}", exc_info=True)
        else:
            self.logger.info("Running in development mode - using mock responses")

    def get_subscriptions(self) -> Dict[str, Callable]:
        """Subscribe to T2T input channels."""
        return {
            Config.T2T_TRANSLATION_IN: self.handle_message_safely(self.handle_translation_input),
            Config.T2T_MODELS: self.handle_message_safely(self.handle_models_request)
        }

    async def initialize(self):
        """Initialize T2T service and load model list."""
        self.logger.info('Initializing T2T service...')
        self.available_models = ModelLoader.convert_translation_models_to_api_format(self.model_config)
        self.logger.info(f'Available models: {[m["name"] for m in self.available_models]}')

    # -------------------------------------------------------------------------
    # Resource management
    # -------------------------------------------------------------------------

    async def _close_wrapper(self, reason: str = ""):
        """Close the singleton wrapper and clear the reference (caller holds lock or not needed)."""
        if self.t2t_wrapper is not None:
            try:
                self.logger.info(f"Closing T2T wrapper (key: {self.current_wrapper_key}){' — ' + reason if reason else ''}")
                self.t2t_wrapper.close()
                self.logger.info("T2T wrapper closed")
            except Exception as e:
                self.logger.error(f"Error closing T2T wrapper: {e}", exc_info=True)
            finally:
                self.t2t_wrapper = None
                self.current_wrapper_key = None

    async def cleanup_resources_for_service_switch(self):
        """Cleanup T2T resources when switching to another service."""
        self.logger.info('Cleaning up T2T resources for service switch...')

        if Config.T2T_KEEPALIVE_SINGLETON:
            self.logger.info("T2T_KEEPALIVE_SINGLETON is true — skipping wrapper close")
            return

        async with self.t2t_wrapper_lock:
            await self._close_wrapper("service switch")

        import gc
        gc.collect()
        self.logger.info("Waiting for resources to be fully released...")
        await asyncio.sleep(2.0)
        self.logger.info("T2T resources released for service switch")

    async def handle_close_request(self):
        """Handle a translation_close message: close the singleton wrapper and release resources."""
        self.logger.info('T2T close request received — closing singleton wrapper')

        async with self.t2t_wrapper_lock:
            await self._close_wrapper("explicit close")

        # Mark keep_alive as False so the next service switch triggers a full cleanup
        self.coordinator.set_t2t_keep_alive(False)
        self.logger.info("T2T close complete")

    async def cleanup(self):
        """Cleanup T2T resources on service shutdown."""
        self.logger.info('Cleaning up T2T service...')
        async with self.t2t_wrapper_lock:
            await self._close_wrapper("service shutdown")
        self.logger.info("T2T service cleaned up successfully")

    # -------------------------------------------------------------------------
    # Message routing
    # -------------------------------------------------------------------------

    def handle_translation_input(self, message: str):
        """Handle incoming translation requests or control messages."""
        try:
            msg_type = get_message_type(message)
            if msg_type == 'translation_close':
                asyncio.create_task(self.handle_close_request())
                return
            # All other messages are treated as translation requests
            asyncio.create_task(self.handle_translate_request(message))
        except Exception as e:
            self.logger.error(f'Error handling translation input: {e}', exc_info=True)

    # -------------------------------------------------------------------------
    # Translation request handling
    # -------------------------------------------------------------------------

    async def handle_translate_request(self, message: str):
        """Handle a translation request."""
        try:
            # Parse the request first so keep_alive is set before request_service_start
            # runs — the cleanup callback reads keep_alive, so it must be current.
            request = TranslationRequest.from_json(message)
            self.coordinator.set_t2t_keep_alive(request.keep_alive)
            
            #handle translation tag 
            supported_source_langs = {
                mc.get("source_languages", [{}])[0].get("code", "")
                for mc in self.model_config if mc.get("source_languages")
            }
            if request.source_language not in supported_source_langs:
                await self.send_error(
                    Config.T2T_TRANSLATION_OUT,
                    f'Source language "{request.source_language}" is not supported. '
                    f'Supported: {sorted(supported_source_langs)}',
                    sync_id=request.sync_id, code="400", param='source_language'
                )
                return
            # Request service start — cleans up ASR/TTS if they are active
            await self.coordinator.request_service_start(
                'T2T',
                self.cleanup_resources_for_service_switch
            )

            self.logger.info(
                f'Translation request: {request.source_language} -> {request.target_language}, '
                f'{len(request.text)} text(s), keep_alive={request.keep_alive}'
            )

            # Resolve model name
            model_name = await self._resolve_model(request)
            if model_name is None:
                return  # error already sent
            request.model = model_name

            # Translate
            code, result = await self.translate_texts(
                request.text,
                request.source_language,
                request.target_language,
                request.model,
                request.parameters
            )

            if code != 0:
                # result is an error message string
                self.logger.error(f'Translation failed (code {code}): {result}')
                await self.send_error(
                    Config.T2T_TRANSLATION_OUT,
                    result,
                    sync_id=request.sync_id
                )
                return

            response = TranslationResponse.create_response(
                sync_id=request.sync_id,
                translations=result
            )
            await self.publish(Config.T2T_TRANSLATION_OUT, response.to_json())
            self.logger.info(f'Sent translation response with {len(result)} result(s)')

        except Exception as e:
            self.logger.error(f'Error handling translate request: {e}', exc_info=True)
            await self.send_error(
                Config.T2T_TRANSLATION_OUT,
                str(e),
                sync_id=getattr(request, 'sync_id', None) if 'request' in locals() else None
            )

    async def _resolve_model(self, request: TranslationRequest):
        """
        Resolve the model name from the request, auto-selecting by language pair if needed.
        Returns the model name, or None if an error was sent.
        """
        model_name = request.model

        if not model_name:
            # Auto-select by language pair
            for mc in self.model_config:
                src_langs = mc.get("source_languages", [])
                tgt_langs = mc.get("target_languages", [])
                src = src_langs[0].get("code", "") if src_langs else ""
                tgt = tgt_langs[0].get("code", "") if tgt_langs else ""
                if src == request.source_language and tgt == request.target_language:
                    self.logger.info(f"Auto-selected model {mc['name']} for {src}->{tgt}")
                    return mc["name"]

            supported_pairs = [
                (mc.get("source_languages", [{}])[0].get("code"),
                 mc.get("target_languages", [{}])[0].get("code"))
                for mc in self.model_config
            ]
            await self.send_error(
                Config.T2T_TRANSLATION_OUT,
                f'Translation from "{request.source_language}" to "{request.target_language}" '
                f'is not supported. Supported pairs: {supported_pairs}',
                sync_id=request.sync_id,
                param='language'
            )
            return None

        # Verify the specified model exists
        mc = next((m for m in self.model_config if m["name"] == model_name), None)
        if mc is None:
            available = [m["name"] for m in self.model_config]
            await self.send_error(
                Config.T2T_TRANSLATION_OUT,
                f'Translation model "{model_name}" is not available. '
                f'Available models: {", ".join(available)}',
                sync_id=request.sync_id,
                param='model'
            )
            return None

        # Verify language pair
        src_langs = mc.get("source_languages", [])
        tgt_langs = mc.get("target_languages", [])
        src = src_langs[0].get("code", "") if src_langs else ""
        tgt = tgt_langs[0].get("code", "") if tgt_langs else ""
        if src != request.source_language or tgt != request.target_language:
            await self.send_error(
                Config.T2T_TRANSLATION_OUT,
                f'Model "{model_name}" supports {src}->{tgt}, not '
                f'"{request.source_language}"->"{request.target_language}".',
                sync_id=request.sync_id,
                param='language'
            )
            return None

        return model_name

    # -------------------------------------------------------------------------
    # Translation engine
    # -------------------------------------------------------------------------

    async def translate_texts(
        self,
        texts: List[str],
        source_lang: str,
        target_lang: str,
        model: str,
        parameters: Dict = None
    ):
        """
        Translate a list of texts using the singleton TranslationWrapper.

        Returns:
            (0, List[TranslationResult])  on success
            (error_code, error_message)   on failure  (error_code != 0)
        """
        ERROR_ENGINE_UNAVAILABLE = 1
        ERROR_MODEL_NOT_FOUND    = 2
        ERROR_INIT_FAILED        = 3
        ERROR_ENGINE_ERROR       = 4
        ERROR_NO_RESULT          = 5

        if self.dev_mode:
            self.logger.info("Using mock translations (dev mode)")
            await asyncio.sleep(0.3)
            return (0, [
                TranslationResult(
                    translated_text="转录的文本：天空是蓝色的。",
                    target_language=target_lang,
                    source_language=source_lang
                )
                for _ in texts
            ])

        if not self.t2t_engine:
            msg = 'Translation engine is not available. The wrapper failed to load at startup.'
            self.logger.error(msg)
            return (ERROR_ENGINE_UNAVAILABLE, msg)

        # Find model config
        model_config = next((m for m in self.model_config if m["name"] == model), None)
        if not model_config:
            msg = f'Internal error: model configuration not found for "{model}".'
            self.logger.error(msg)
            return (ERROR_MODEL_NOT_FOUND, msg)

        # Get the model directory path (injected by ModelLoader)
        model_dir = model_config.get("model_path", "")
        if not model_dir:
            msg = f'Internal error: model directory not configured for "{model}".'
            self.logger.error(msg)
            return (ERROR_MODEL_NOT_FOUND, msg)

        # Map language codes to names expected by the C++ engine
        lang_name_map = {
            'en': 'English', 'es': 'Spanish', 'fr': 'French', 'de': 'German',
            'zh': 'Chinese', 'ja': 'Japanese', 'ko': 'Korean', 'ar': 'Arabic',
            'ru': 'Russian', 'pt': 'Portuguese', 'it': 'Italian'
        }
        input_lang = lang_name_map.get(source_lang, source_lang).encode('utf-8')
        output_lang = lang_name_map.get(target_lang, target_lang).encode('utf-8')

        wrapper_key = f"{model}_{input_lang.decode()}_{output_lang.decode()}"
        self.logger.info(f"Wrapper key: {wrapper_key}")

        # Get or create the singleton wrapper
        async with self.t2t_wrapper_lock:
            if self.t2t_wrapper is None or self.current_wrapper_key != wrapper_key:
                if self.t2t_wrapper is not None:
                    self.logger.info(
                        f"Model/language changed from '{self.current_wrapper_key}' to "
                        f"'{wrapper_key}' — reinitializing T2T wrapper"
                    )
                    await self._close_wrapper("model/language change")

                self.logger.info("Waiting 1 s before creating new wrapper...")
                await asyncio.sleep(1.0)

                try:
                    self.logger.info(f"Creating singleton T2T wrapper for key: {wrapper_key}")
                    self.t2t_wrapper = self.t2t_engine.TranslationWrapper(
                        model_path=None,
                        model_dir=model_dir,
                        input_lang=input_lang,
                        output_lang=output_lang
                    )
                    if self.t2t_wrapper is None:
                        raise RuntimeError("TranslationWrapper constructor returned None")
                    self.current_wrapper_key = wrapper_key
                    self.logger.info(f"Singleton T2T wrapper created: {wrapper_key}")
                except Exception as e:
                    self.logger.error(f"Error creating T2T wrapper: {e}", exc_info=True)
                    self.t2t_wrapper = None
                    self.current_wrapper_key = None
                    msg = f'Translation engine failed to initialize for model "{model}": {e}'
                    return (ERROR_INIT_FAILED, msg)
            else:
                self.logger.info(f"Reusing existing singleton T2T wrapper: {wrapper_key}")

            wrapper = self.t2t_wrapper

        # Model is managed via model_dir — no additional model check needed

        # Translate each text
        translations = []
        for i, text in enumerate(texts):
            self.logger.info(f"Translating text {i+1}/{len(texts)}: {text[:100]}...")
            result_parts = []
            error_code = [None]
            done_flag = [False]

            def on_result(chunk: str):
                result_parts.append(chunk)

            def on_done():
                done_flag[0] = True

            def on_error(code: int):
                error_code[0] = code

            ret = wrapper.process_with_cb(text, on_result, on_done, on_error)

            if ret != 0 or error_code[0] is not None:
                code = ret if ret != 0 else error_code[0]
                self.logger.error(f"Translation failed (code {code}) — invalidating wrapper")
                async with self.t2t_wrapper_lock:
                    await self._close_wrapper("translation error")
                msg = f'Translation engine returned error code {code} for model "{model}".'
                return (ERROR_ENGINE_ERROR, msg)

            if not result_parts:
                self.logger.error("No translation result received — invalidating wrapper")
                async with self.t2t_wrapper_lock:
                    await self._close_wrapper("empty result")
                msg = f'Translation engine returned no result for model "{model}".'
                return (ERROR_NO_RESULT, msg)

            full_translation = ' '.join(result_parts).strip()
            self.logger.info(f"Translated: '{text[:60]}' -> '{full_translation[:60]}'")
            translations.append(TranslationResult(
                translated_text=full_translation,
                target_language=target_lang,
                source_language=source_lang
            ))

        self.logger.info(f"Keeping singleton T2T wrapper alive for reuse (key: {wrapper_key})")
        return (0, translations)

    # -------------------------------------------------------------------------
    # Models request
    # -------------------------------------------------------------------------

    def handle_models_request(self, message: str):
        """Handle models list request."""
        try:
            import json
            data = json.loads(message)
            if data.get('message_source') == 'audio_analytics_server':
                return

            request = TranslationModelsRequest.from_json(message)
            response = {
                "sync_id": request.sync_id,
                "result": self.available_models,
                "message_source": "audio_analytics_server"
            }
            asyncio.create_task(
                self.publish(Config.T2T_MODELS, json.dumps(response))
            )
            self.logger.info(f'Sent models list: {len(self.available_models)} models')

        except Exception as e:
            self.logger.error(f'Error handling models request: {e}', exc_info=True)

    
