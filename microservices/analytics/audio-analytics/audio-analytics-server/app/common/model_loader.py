# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import json
from typing import Dict, Any, Optional, List
from common.logger import get_logger

logger = get_logger(__name__)

class ModelLoader:
    """
    Utility class to load model configurations by scanning subdirectories
    under ASR_MODELS_DIR, TRANSLATION_MODELS_DIR, and TTS_MODELS_DIR.

    Each subdirectory is expected to contain a config.json that describes
    the model.  The absolute path of the subdirectory is injected into the
    loaded config as ``model_path`` so the engine wrappers can locate the
    binary assets without any additional path resolution.

    Environment Variables:
        MODELS_DIR              Base models directory (default: /home/ubuntu/models/audio)
        ASR_MODELS_DIR          ASR models directory  (default: <MODELS_DIR>/asr)
        TRANSLATION_MODELS_DIR  Translation models directory (default: <MODELS_DIR>/translation)
        TTS_MODELS_DIR          TTS models directory  (default: <MODELS_DIR>/tts)
    """
    
    @staticmethod
    def _get_models_dir() -> str:
        return os.getenv('MODELS_DIR', '/home/ubuntu/models/audio')

    @staticmethod
    def _load_models_from_dir(service_dir: str) -> List[Dict[str, Any]]:
        """
        Scan *service_dir* for subdirectories that contain a config.json.
        Each config.json is read and the absolute subdirectory path is
        injected as ``model_path`` so engine wrappers can find the assets.

        Returns a list of model config dicts (one per subdirectory).
        """
        models = []

        if not service_dir or not os.path.isdir(service_dir):
            logger.warning(f"Models directory not found or not a directory: {service_dir}")
            return models

        try:
            entries = sorted(os.listdir(service_dir))
        except OSError as e:
            logger.error(f"Cannot list models directory '{service_dir}': {e}")
            return models

        for entry in entries:
            subdir = os.path.join(service_dir, entry)
            if not os.path.isdir(subdir):
                continue

            config_path = os.path.join(subdir, 'config.json')
            if not os.path.exists(config_path):
                logger.debug(f"Skipping '{subdir}' — no config.json found")
                continue

            try:
                with open(config_path, 'r') as f:
                    config = json.load(f)
                # Inject the absolute model directory path so engine wrappers
                # can resolve asset filenames without extra path logic.
                config['model_path'] = subdir
                # TTS voices are looked up individually by the engine — propagate
                # model_path into each voice so synthesize_speech_real can find it
                # via voice_config.get("model_path").
                for voice in config.get('voices', []):
                    voice['model_path'] = subdir
                models.append(config)
                logger.info(f"Loaded model config: {config.get('name', entry)} from {subdir}")
            except Exception as e:
                logger.error(f"Error reading config.json in '{subdir}': {e}", exc_info=True)

        return models

    @staticmethod
    def get_asr_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Scan ASR_MODELS_DIR for model subdirectories and return their configs.
        """
        base = ModelLoader._get_models_dir()
        asr_dir = os.getenv('ASR_MODELS_DIR', os.path.join(base, 'asr'))
        logger.info(f"Loading ASR models from: {asr_dir}")
        return ModelLoader._load_models_from_dir(asr_dir)

    @staticmethod
    def get_translation_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Scan TRANSLATION_MODELS_DIR for model subdirectories and return their configs.
        """
        base = ModelLoader._get_models_dir()
        translation_dir = os.getenv('TRANSLATION_MODELS_DIR', os.path.join(base, 'translation'))
        logger.info(f"Loading translation models from: {translation_dir}")
        return ModelLoader._load_models_from_dir(translation_dir)

    @staticmethod
    def get_tts_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Scan TTS_MODELS_DIR for model subdirectories and return their configs.
        """
        base = ModelLoader._get_models_dir()
        tts_dir = os.getenv('TTS_MODELS_DIR', os.path.join(base, 'tts'))
        logger.info(f"Loading TTS models from: {tts_dir}")
        return ModelLoader._load_models_from_dir(tts_dir)
    
    @staticmethod
    def convert_translation_models_to_api_format(models: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """
        Convert translation models to the format expected by the API.
        Each model is returned individually with its specific source and target language.
        
        Args:
            models: List of translation models from the config
            
        Returns:
            List of models in API format, one entry per model
        """
        api_models = []
        
        for model in models:
            # Get source and target languages from arrays
            source_languages = model.get("source_languages", [])
            target_languages = model.get("target_languages", [])
            
            # Extract first language from each array (models have single source/target)
            source_lang = source_languages[0] if source_languages else {"code": "", "name": ""}
            target_lang = target_languages[0] if target_languages else {"code": "", "name": ""}
            
            api_model = {
                "name": model.get("name", ""),
                "display_name": model.get("display_name", model.get("name", "")),
                "version": model.get("version", "1.0.0"),
                "description": model.get("description", ""),
                "source_language": {
                    "code": source_lang.get("code", ""),
                    "name": source_lang.get("name", "")
                },
                "target_language": {
                    "code": target_lang.get("code", ""),
                    "name": target_lang.get("name", "")
                },
                "capabilities": model.get("capabilities", {}),
                "parameters": model.get("parameters", {})
            }
            
            api_models.append(api_model)
        
        return api_models
    
    @staticmethod
    def convert_tts_models_to_api_format(models: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
        """
        Convert TTS models to the format expected by the API.
        
        Args:
            models: List of TTS models from the config
            
        Returns:
            List of models in API format
        """
        api_models = []
        
        for model in models:
            api_model = {
                "name": model["name"],
                "display_name": model.get("display_name", model["name"]),
                "description": model.get("description", ""),
                "voices": [],
                "capabilities": model.get("capabilities", {}),
                "parameters": model.get("parameters", {})
            }
            
            for voice in model.get("voices", []):
                api_voice = {
                    "name": voice["name"],
                    "display_name": voice.get("display_name", voice["name"]),
                    "language": voice["language"],
                    "language_name": voice.get("language_name", voice["language"]),
                    "gender": voice.get("gender", "neutral"),
                    "style": voice.get("style", "neutral"),
                    "sample_rate": voice.get("sample_rate", 44100),
                    "description": voice.get("description", "")
                }
                api_model["voices"].append(api_voice)
            
            api_models.append(api_model)
        
        return api_models
