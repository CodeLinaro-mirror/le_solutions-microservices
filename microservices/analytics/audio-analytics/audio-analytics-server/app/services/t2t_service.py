# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import Dict, Callable, List
import asyncio
import os
import sys
import importlib.util
import ctypes
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
    Handles translation requests between different languages.
    """
    
    def __init__(self, redis_client: RedisClient):
        super().__init__(redis_client, "T2T")
        self.dev_mode = Config.DEV_MODE
        self.model_config = ModelLoader.get_translation_models(self.dev_mode)
        self.available_models = []  # Will be populated during initialization
        
        # Initialize T2T engine if not in dev mode
        self.t2t_engine = None
        self.t2t_wrapper = None
        self.engine_handle = None
        self.callback_handle = None
        self.translation_results = {}
        
        # Use process manager for separate worker processes
        self.process_manager = None
        self.use_process_mode = True  # Flag to enable process-based translation
        
        # Keep old wrapper system as fallback
        self.translation_wrappers = {}  # Dict to store wrapper instances by model key
        self.current_wrapper_key = None
        self.wrapper_was_cleaned_up = False  # Track if wrapper was cleaned up
        
        # Service coordinator for managing resource conflicts with ASR
        self.coordinator = get_service_coordinator()
        
        if not self.dev_mode:
            self.logger.info("Running in production mode - initializing T2T engine")
            
            if self.use_process_mode:
                self.logger.info("Using process-based translation mode")
                try:
                    # Import process manager
                    engine_path = os.path.join(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "engine", "python"
                    )
                    if engine_path not in sys.path:
                        sys.path.insert(0, engine_path)
                    
                    from translation_process_manager import get_process_manager
                    self.process_manager = get_process_manager()
                    self.logger.info("Process manager initialized successfully")
                    
                except Exception as e:
                    self.logger.error(f"Error initializing process manager: {e}", exc_info=True)
                    self.logger.info("Falling back to wrapper mode")
                    self.use_process_mode = False
            
            if not self.use_process_mode:
                self.logger.info("Using wrapper-based translation mode")
                try:
                    # Try to import the translation wrapper module from engine/python directory
                    engine_path = os.path.join(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "engine", "python"
                    )
                    wrapper_path = os.path.join(engine_path, "translate_wrapper.py")
                    
                    if os.path.exists(wrapper_path):
                        self.logger.info(f"Found translation wrapper at {wrapper_path}")
                        # Add engine path to sys.path if not already there
                        if engine_path not in sys.path:
                            sys.path.insert(0, engine_path)
                        
                        # Import the module dynamically
                        spec = importlib.util.spec_from_file_location("translate_wrapper", wrapper_path)
                        if spec and spec.loader:
                            self.t2t_engine = importlib.util.module_from_spec(spec)
                            sys.modules["translate_wrapper"] = self.t2t_engine
                            spec.loader.exec_module(self.t2t_engine)
                            self.logger.info("Translation wrapper module loaded successfully")
                            
                            # Check if TranslationWrapper class is available
                            if hasattr(self.t2t_engine, 'TranslationWrapper'):
                                self.logger.info("TranslationWrapper class found successfully")
                            else:
                                self.logger.warning("TranslationWrapper class not found in wrapper module")
                        else:
                            self.logger.error("Failed to load translation wrapper module specification")
                    else:
                        self.logger.warning(f"Translation wrapper not found at {wrapper_path}")
                except Exception as e:
                    self.logger.error(f"Error initializing translation wrapper: {e}", exc_info=True)
        else:
            self.logger.info("Running in development mode - using mock responses")
        
    def get_subscriptions(self) -> Dict[str, Callable]:
        """Subscribe to T2T input channels."""
        return {
            Config.T2T_TRANSLATION_IN: self.handle_message_safely(self.handle_translation_input),
            Config.T2T_MODELS: self.handle_message_safely(self.handle_models_request)
        }
    
    async def initialize(self):
        """Initialize T2T engine and load models."""
        self.logger.info('Initializing T2T service...')
        
        if not self.dev_mode and self.t2t_engine:
            try:
                # The translation wrapper uses the TranslationWrapper class
                # We don't need to initialize it here, we'll create instances per request
                # Just verify the wrapper is available
                if hasattr(self.t2t_engine, 'TranslationWrapper'):
                    self.logger.info("TranslationWrapper class is available")
                else:
                    self.logger.warning("TranslationWrapper class not found in wrapper module")
                
                # Convert the model config to the format expected by the API
                self.available_models = ModelLoader.convert_translation_models_to_api_format(self.model_config)
                
                self.logger.info("T2T wrapper initialized successfully")
            except Exception as e:
                self.logger.error(f"Error initializing T2T wrapper: {e}", exc_info=True)
                # Fall back to mock models
                self.setup_mock_models()
        else:
            # In development mode, use mock models
            self.setup_mock_models()
        
        self.logger.info(f'Available models: {[m["name"] for m in self.available_models]}')
    
    def setup_mock_models(self):
        """Set up translation models for the API format."""
        # Convert the model config to the format expected by the API
        self.available_models = ModelLoader.convert_translation_models_to_api_format(self.model_config)
    
    async def cleanup_resources_for_service_switch(self):
        """Cleanup T2T resources when switching to another service."""
        
        self.logger.info('Cleaning up T2T resources for service switch...')
        
        if Config.T2T_KEEPALIVE_SINGLETON:
            self.logger.info("T2T_KEEPALIVE_SINGLETON is true")
            return
        
        if self.use_process_mode and self.process_manager:
            self.logger.info("Stopping all translation worker processes for service switch...")
            try:
                self.process_manager.stop_all_workers()
                self.logger.info("All translation worker processes stopped")
            except Exception as e:
                self.logger.error(f"Error stopping worker processes: {e}")
        else:
            # Clean up all translation wrappers if they exist
            if self.translation_wrappers:
                self.logger.info(f"Destroying {len(self.translation_wrappers)} translation wrapper instances for service switch...")
                
                for wrapper_key, wrapper in list(self.translation_wrappers.items()):
                    try:
                        self.logger.info(f"Closing wrapper for key: {wrapper_key}")
                        wrapper.close()
                        self.logger.info(f"Translation wrapper {wrapper_key} closed successfully")
                    except Exception as e:
                        self.logger.error(f"Error closing translation wrapper {wrapper_key}: {e}")
                
                # Clear all references
                self.translation_wrappers.clear()
                self.current_wrapper_key = None
            
            # Force garbage collection to ensure resources are freed
            import gc
            gc.collect()
            
            # Add a delay to ensure resources are fully released
            import asyncio
            self.logger.info("Waiting for resources to be fully released...")
            await asyncio.sleep(2.0)
            
            self.logger.info("T2T resources released for service switch")
            
            # Note: We keep the global callback system intact since it's shared
            # Only the individual wrapper instances are destroyed
            self.logger.info("Global callback system preserved for reuse by new instances")
            
            self.logger.info("T2T resources fully released for service switch")
    
    async def cleanup(self):
        """Cleanup T2T resources."""
        self.logger.info('Cleaning up T2T service...')
        
        if self.use_process_mode and self.process_manager:
            self.logger.info("Stopping all translation worker processes...")
            try:
                self.process_manager.stop_all_workers()
                self.logger.info("All translation worker processes stopped")
            except Exception as e:
                self.logger.error(f"Error stopping worker processes: {e}")
        else:
            # Clean up all translation wrappers if they exist
            if self.translation_wrappers:
                self.logger.info(f"Cleaning up {len(self.translation_wrappers)} translation wrapper instances...")
                
                for wrapper_key, wrapper in list(self.translation_wrappers.items()):
                    try:
                        self.logger.info(f"Closing wrapper for key: {wrapper_key}")
                        wrapper.close()
                        self.logger.info(f"Translation wrapper {wrapper_key} closed successfully")
                    except Exception as e:
                        self.logger.error(f"Error closing translation wrapper {wrapper_key}: {e}")
                
                # Clear all references
                self.translation_wrappers.clear()
                self.current_wrapper_key = None
        
        self.logger.info("T2T service cleaned up successfully")
    
    def handle_translation_input(self, message: str):
        """Handle incoming translation requests."""
        try:
            # Translation requests don't have message_type, they're direct requests
            asyncio.create_task(self.handle_translate_request(message))
        except Exception as e:
            self.logger.error(f'Error handling translation input: {e}', exc_info=True)
    
    async def handle_translate_request(self, message: str):
        """Handle translation request."""
        try:
            # Request service start - this will cleanup ASR if it's active
            await self.coordinator.request_service_start(
                'T2T',
                self.cleanup_resources_for_service_switch
            )
            
            request = TranslationRequest.from_json(message)
            self.logger.info(
                f'Translation request: {request.source_language} -> {request.target_language}, '
                f'{len(request.text)} texts'
            )
            
            # Find the appropriate model for the language pair
            model_found = False
            model_name = request.model
            
            # If model is not specified, find a model that supports the language pair
            if not model_name:
                for model_config in self.model_config:
                    # Get source and target languages from arrays
                    source_languages = model_config.get("source_languages", [])
                    target_languages = model_config.get("target_languages", [])
                    
                    # Extract first language from each array (models have single source/target)
                    source_lang = source_languages[0].get("code", "") if source_languages else ""
                    target_lang = target_languages[0].get("code", "") if target_languages else ""
                    
                    if source_lang == request.source_language and target_lang == request.target_language:
                        model_name = model_config["name"]
                        model_found = True
                        self.logger.info(f"Found model {model_name} for {source_lang} to {target_lang}")
                        break
                
                if not model_found:
                    # Check if the languages are supported at all
                    supported_pairs = [(m.get("source_language", {}).get("code"), 
                                      m.get("target_language", {}).get("code")) 
                                     for m in self.model_config]
                    await self.send_error(
                        Config.T2T_TRANSLATION_OUT,
                        f'Translation from "{request.source_language}" to "{request.target_language}" is not supported. '
                        f'Supported language pairs: {supported_pairs}',
                        sync_id=request.sync_id,
                        param='language'
                    )
                    return
            else:
                # Check if the specified model exists
                model_found = any(m["name"] == model_name for m in self.model_config)
                
                if not model_found:
                    available_models = [m["name"] for m in self.model_config]
                    await self.send_error(
                        Config.T2T_TRANSLATION_OUT,
                        f'Translation model "{model_name}" is not available. '
                        f'Available models: {", ".join(available_models)}',
                        sync_id=request.sync_id,
                        param='model'
                    )
                    return
                
                # Verify the model supports the requested language pair
                model_config = next((m for m in self.model_config if m["name"] == model_name), None)
                if model_config:
                    # Get source and target languages from arrays
                    source_languages = model_config.get("source_languages", [])
                    target_languages = model_config.get("target_languages", [])
                    
                    # Extract first language from each array (models have single source/target)
                    source_lang = source_languages[0].get("code", "") if source_languages else ""
                    target_lang = target_languages[0].get("code", "") if target_languages else ""
                    
                    if source_lang != request.source_language or target_lang != request.target_language:
                        await self.send_error(
                            Config.T2T_TRANSLATION_OUT,
                            f'Model "{model_name}" does not support translation from '
                            f'"{request.source_language}" to "{request.target_language}". '
                            f'This model supports: {source_lang} to {target_lang}',
                            sync_id=request.sync_id,
                            param='language'
                        )
                        return
            
            # Update the request model
            request.model = model_name
            
            # Translate texts
            self.logger.info("Starting translation process...")
            translations = await self.translate_texts(
                request.text,
                request.source_language,
                request.target_language,
                request.model,
                request.parameters
            )
            self.logger.info(f"Translation process completed, got {len(translations)} results")
            
            # Send response
            self.logger.info("Creating translation response...")
            response = TranslationResponse.create_response(
                sync_id=request.sync_id,
                translations=translations
            )
            self.logger.info(f"Created response with sync_id: {request.sync_id}")
            
            self.logger.info(f"Publishing response to {Config.T2T_TRANSLATION_OUT}...")
            await self.publish(Config.T2T_TRANSLATION_OUT, response.to_json())
            self.logger.info(f'Sent translation response with {len(translations)} results')
            
        except Exception as e:
            self.logger.error(f'Error handling translate request: {e}', exc_info=True)
            await self.send_error(
                Config.T2T_TRANSLATION_OUT,
                str(e),
                sync_id=getattr(request, 'sync_id', None) if 'request' in locals() else None
            )
    
    async def translate_texts(
        self,
        texts: List[str],
        source_lang: str,
        target_lang: str,
        model: str,
        parameters: Dict = None
    ) -> List[TranslationResult]:
        """
        Translate a list of texts.
        
        Args:
            texts: List of texts to translate
            source_lang: Source language code
            target_lang: Target language code
            model: Model to use
            parameters: Optional translation parameters
            
        Returns:
            List of TranslationResult objects
        """
        if self.dev_mode:
            # In development mode, use mock translations
            self.logger.info("Using mock translations")
            await asyncio.sleep(0.3)  # Simulate processing time
            
            # Mock translations
            translations = []
            for text in texts:
                # Simple mock: return a sample Chinese translation
                translated_text = "转录的文本：天空是蓝色的。"
                
                translations.append(TranslationResult(
                    translated_text=translated_text,
                    target_language=target_lang,
                    source_language=source_lang
                ))
            
            return translations
        elif self.use_process_mode and self.process_manager:
            # Use process-based translation
            self.logger.info("Using process-based translation engine")
            return await self.translate_texts_with_processes(texts, source_lang, target_lang, model, parameters)
        elif self.use_process_mode and not self.process_manager:
            # Process mode enabled but manager failed to initialize
            self.logger.error("Process mode enabled but process manager is None - initialization failed")
            raise RuntimeError('Translation engine is not available. The process manager failed to initialize at startup.')
        else:
            # In production mode, use the actual translation engine with wrappers
            self.logger.info("Using wrapper-based translation engine")
            translations = []
            
            try:
                # Find the model configuration
                model_config = next((m for m in self.model_config if m["name"] == model), None)
                if not model_config:
                    self.logger.error(f"Model configuration not found for {model}")
                    raise RuntimeError(f'Internal error: model configuration not found for "{model}".')
                
                # Get the model path
                model_path = model_config.get("model_path", "").encode('utf-8')
                if not model_path:
                    self.logger.error(f"Model path not found for {model}")
                    raise RuntimeError(f'Internal error: model path not configured for "{model}".')
                
                # Get the language names (the wrapper expects language names, not codes)
                # Map language codes to names
                lang_name_map = {
                    'en': 'English',
                    'es': 'Spanish',
                    'fr': 'French',
                    'de': 'German',
                    'zh': 'Chinese',
                    'ja': 'Japanese',
                    'ko': 'Korean',
                    'ar': 'Arabic',
                    'ru': 'Russian',
                    'pt': 'Portuguese',
                    'it': 'Italian'
                }
                
                input_lang = lang_name_map.get(source_lang, source_lang).encode('utf-8')
                output_lang = lang_name_map.get(target_lang, target_lang).encode('utf-8')
                
                self.logger.info(f"Using languages: {source_lang} ({input_lang}) -> {target_lang} ({output_lang})")
                self.logger.info(f"Model path: {model_path}")
                
                # Create a unique key for this model configuration
                wrapper_key = f"{model}_{input_lang.decode()}_{output_lang.decode()}"
                self.logger.info(f"Wrapper key: {wrapper_key}")
                
                # Check if we already have a wrapper for this configuration
                if wrapper_key in self.translation_wrappers:
                    self.logger.info(f"Reusing existing wrapper for key: {wrapper_key}")
                    wrapper = self.translation_wrappers[wrapper_key]
                    self.current_wrapper_key = wrapper_key
                    
                    # Verify the wrapper is configured for the correct model/languages
                    self.logger.info(f"Verifying wrapper configuration:")
                    self.logger.info(f"  Expected: {model_path} | {input_lang} -> {output_lang}")
                    self.logger.info(f"  Wrapper has: {wrapper.current_model_path} | {wrapper.current_input_lang} -> {wrapper.current_output_lang}")
                    
                    # The wrapper key should guarantee the right model, but let's be safe
                    # Don't check wrapper's stored values here since _ensure_correct_model will handle it
                else:
                    self.logger.info(f"Creating new wrapper for key: {wrapper_key}")
                    wrapper = None  # Will be created below
                
                # Create new wrapper if needed
                if wrapper is None:
                    # Add delay before creating new wrapper to ensure resources are available
                    self.logger.info("Waiting before creating new wrapper (1 second)...")
                    await asyncio.sleep(1.0)
                    
                    try:
                        wrapper = self.t2t_engine.TranslationWrapper(
                            model_path=model_path,
                            input_lang=input_lang,
                            output_lang=output_lang
                        )
                        
                        # Verify the wrapper was created successfully
                        if wrapper is None:
                            raise RuntimeError("Failed to create TranslationWrapper - returned None")
                            
                        # Store the new wrapper instance
                        self.translation_wrappers[wrapper_key] = wrapper
                        self.current_wrapper_key = wrapper_key
                        self.logger.info(f"TranslationWrapper created and stored for key: {wrapper_key}")
                    except Exception as e:
                        self.logger.error(f"Error creating new wrapper for key {wrapper_key}: {e}", exc_info=True)
                        raise RuntimeError(f'Translation engine failed to initialize for model "{model}": {e}')
                
                # Verify we have a valid wrapper before proceeding
                if wrapper is None:
                    self.logger.error(f"Translation wrapper is None for key {wrapper_key}, cannot proceed with translation")
                    raise RuntimeError(f'Internal error: translation wrapper could not be created for model "{model}".')
                
                # Ensure wrapper has correct model before processing
                # Pass the EXPECTED model parameters, not the wrapper's stored ones
                if hasattr(wrapper, '_ensure_correct_model'):
                    self.logger.info(f"Ensuring wrapper {wrapper_key} has correct model: {model_path} {input_lang}->{output_lang}")
                    if not wrapper._ensure_correct_model(model_path, input_lang, output_lang):
                        self.logger.error(f"Failed to set correct model on wrapper {wrapper_key}")
                        raise RuntimeError(f'Internal error: failed to configure translation model "{model}" for {source_lang} -> {target_lang}.')
                
                # Process each text using the new callback-based approach
                for i, text in enumerate(texts):
                    try:
                        self.logger.info(f"Processing text {i+1}/{len(texts)}: {text[:100]}...")
                        # Storage for the translation result - use list to collect all parts
                        translation_result = {'text_parts': [], 'error': None, 'done': False}
                        
                        # Define callback functions for this translation
                        def on_result(result_text: str):
                            """Callback to receive translation result."""
                            self.logger.info(f"Translation result chunk received: {result_text}")
                            # Append each result chunk to the list
                            translation_result['text_parts'].append(result_text)
                        
                        def on_done():
                            """Callback when translation is complete."""
                            self.logger.info("Translation done callback called")
                            translation_result['done'] = True
                        
                        def on_error(error_code: int):
                            """Callback when translation error occurs."""
                            self.logger.error(f"Translation error: code {error_code}")
                            translation_result['error'] = error_code
                        
                        # Process the text with callbacks
                        self.logger.info(f"Calling process_with_cb for text: '{text}'")
                        ret = wrapper.process_with_cb(text, on_result, on_done, on_error)
                        self.logger.info(f"process_with_cb returned: {ret}")
                        
                        if ret != 0:
                            self.logger.error(f"Translation process failed with code {ret}")
                            self.logger.error(f"Wrapper {wrapper_key} is in bad state, will remove and recreate on next request")
                            # Remove the bad wrapper
                            try:
                                wrapper.close()
                            except:
                                pass
                            if wrapper_key in self.translation_wrappers:
                                del self.translation_wrappers[wrapper_key]
                            if self.current_wrapper_key == wrapper_key:
                                self.current_wrapper_key = None
                            raise RuntimeError(f'Translation engine returned error code {ret} for model "{model}".')
                        elif translation_result['error'] is not None:
                            self.logger.error(f"Translation error: {translation_result['error']}")
                            self.logger.error(f"Wrapper {wrapper_key} is in bad state, will remove and recreate on next request")
                            # Remove the bad wrapper
                            try:
                                wrapper.close()
                            except:
                                pass
                            if wrapper_key in self.translation_wrappers:
                                del self.translation_wrappers[wrapper_key]
                            if self.current_wrapper_key == wrapper_key:
                                self.current_wrapper_key = None
                            raise RuntimeError(f'Translation engine error code {translation_result["error"]} for model "{model}".')
                        elif translation_result['text_parts']:
                            # Successfully translated - concatenate all parts
                            full_translation = ' '.join(translation_result['text_parts']).strip()
                            self.logger.info(f"Successfully translated: '{text}' -> '{full_translation}'")
                            self.logger.info(f"Translation had {len(translation_result['text_parts'])} parts: {translation_result['text_parts']}")
                            translations.append(TranslationResult(
                                translated_text=full_translation,
                                target_language=target_lang,
                                source_language=source_lang
                            ))
                        else:
                            self.logger.warning(f"No translation result for text: {text}")
                            self.logger.warning(f"Translation result state: {translation_result}")
                            self.logger.error(f"No results received - wrapper {wrapper_key} may be in bad state, removing")
                            # Remove the bad wrapper since we got no results
                            try:
                                wrapper.close()
                            except:
                                pass
                            if wrapper_key in self.translation_wrappers:
                                del self.translation_wrappers[wrapper_key]
                            if self.current_wrapper_key == wrapper_key:
                                self.current_wrapper_key = None
                            raise RuntimeError(f'Translation engine returned no result for model "{model}".')
                            
                    except Exception as e:
                        self.logger.error(f"Error translating text: {e}", exc_info=True)
                        raise
                
                # We're keeping all wrapper instances for reuse, so don't close them here
                self.logger.info(f"Keeping {len(self.translation_wrappers)} translation wrapper instances for future requests")
                self.logger.info(f"Current wrapper keys: {list(self.translation_wrappers.keys())}")
                
                self.logger.info(f"Returning {len(translations)} translations")
                return translations
                
            except Exception as e:
                self.logger.error(f"Error using translation engine: {e}", exc_info=True)
                raise
    
    async def translate_texts_with_processes(self, texts: List[str], source_lang: str, target_lang: str, model: str, parameters: Dict = None) -> List[TranslationResult]:
        """Translate texts using separate worker processes."""
        try:
            # Find the model configuration
            model_config = next((m for m in self.model_config if m["name"] == model), None)
            if not model_config:
                self.logger.error(f"Model configuration not found for {model}")
                raise RuntimeError(f'Internal error: model configuration not found for "{model}".')
            
            # Get the model path
            model_path = model_config.get("model_path", "")
            if not model_path:
                self.logger.error(f"Model path not found for {model}")
                raise RuntimeError(f'Internal error: model path not configured for "{model}".')
            
            # Map language codes to names for the C++ engine
            lang_name_map = {
                'en': 'English',
                'es': 'Spanish', 
                'fr': 'French',
                'de': 'German',
                'zh': 'Chinese',
                'ja': 'Japanese',
                'ko': 'Korean',
                'ar': 'Arabic',
                'ru': 'Russian',
                'pt': 'Portuguese',
                'it': 'Italian'
            }
            
            input_lang = lang_name_map.get(source_lang, source_lang)
            output_lang = lang_name_map.get(target_lang, target_lang)
            
            self.logger.info(f"Process translation: {model_path} | {input_lang} -> {output_lang}")
            
            translations = []
            
            # Process each text
            for i, text in enumerate(texts):
                try:
                    self.logger.info(f"Processing text {i+1}/{len(texts)} with process manager: {text[:100]}...")
                    
                    # Use process manager to translate
                    result = self.process_manager.translate_text(
                        text=text,
                        model_path=model_path,
                        input_lang=input_lang,
                        output_lang=output_lang,
                        timeout=15.0
                    )
                    
                    if result["error"]:
                        self.logger.error(f"Process translation error: {result['error']}")
                        raise RuntimeError(f'Translation engine error for model "{model}": {result["error"]}')
                    elif result["result"]:
                        self.logger.info(f"Process translation successful: '{text}' -> '{result['result']}'")
                        translations.append(TranslationResult(
                            translated_text=result["result"],
                            target_language=target_lang,
                            source_language=source_lang
                        ))
                    else:
                        self.logger.warning(f"No translation result for text: {text}")
                        raise RuntimeError(f'Translation engine returned no result for model "{model}".')
                        
                except Exception as e:
                    self.logger.error(f"Error processing text with process manager: {e}", exc_info=True)
                    raise
            
            # Log worker status
            try:
                status = self.process_manager.get_worker_status()
                self.logger.info(f"Active workers: {list(status.keys())}")
            except:
                pass
            
            self.logger.info(f"Process-based translation completed, returning {len(translations)} results")
            return translations
            
        except Exception as e:
            self.logger.error(f"Error in process-based translation: {e}", exc_info=True)
            raise
    
    async def translate_texts_mock(self, texts: List[str], source_lang: str, target_lang: str) -> List[TranslationResult]:
        """Generate mock translations for fallback."""
        await asyncio.sleep(0.3)  # Simulate processing time
        
        translations = []
        for text in texts:
            # Simple mock: return a sample Chinese translation
            translated_text = "转录的文本：天空是蓝色的。"
            
            translations.append(TranslationResult(
                translated_text=translated_text,
                target_language=target_lang,
                source_language=source_lang
            ))
        
        return translations
    
    def handle_models_request(self, message: str):
        """Handle models list request."""
        try:
            # Parse the message to check the source
            import json
            data = json.loads(message)
            message_source = data.get('message_source', '')
            
            # Ignore messages from ourselves (responses)
            if message_source == 'audio_analytics_server':
                self.logger.debug('Ignoring message from server (our own response)')
                return
            
            request = TranslationModelsRequest.from_json(message)
            
            # Send models list in the format the API expects
            # The API expects: {"sync_id": "...", "result": [...]}
            response = {
                "sync_id": request.sync_id,
                "result": self.available_models,
                "message_source": "audio_analytics_server"  # Mark as server response
            }
            
            asyncio.create_task(
                self.publish(Config.T2T_MODELS, json.dumps(response))
            )
            
            self.logger.info(f'Sent models list: {len(self.available_models)} models')
            
        except Exception as e:
            self.logger.error(f'Error handling models request: {e}', exc_info=True)
