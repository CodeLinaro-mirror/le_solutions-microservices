# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import json
from typing import Dict, Any, Optional, List
from common.logger import get_logger

logger = get_logger(__name__)

class ModelLoader:
    """
    Utility class to load model configurations from the models.json file.
    """
    
    @staticmethod
    def load_models_config(dev_mode: bool = False) -> Dict[str, Any]:
        """
        Load the models configuration from the models.json file.
        
        Behavior:
        - First checks MODELS_CONFIG environment variable
        - Falls back to MODELS_DIR environment variable + /models.json
        - Then tries default locations
        
        Environment Variables:
        - MODELS_CONFIG: Full path to models.json file (default: /opt/audio/models/models.json)
        - MODELS_DIR: Directory containing models (default: /opt/audio/models)
        
        Returns:
            Dictionary containing the models configuration
        """
        # Default empty configuration
        default_config = {
            "asr": {"default_model": "", "models": []},
            "translation": {"default_model": "", "models": []},
            "tts": {"default_model": "", "models": []}
        }
        
        try:
            # Determine project root (audio-analytics-server)
            project_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
            
            # Get environment variables with defaults
            models_config_path = os.getenv('MODELS_CONFIG', '/opt/audio/models/models.json')
            models_dir = os.getenv('MODELS_DIR', '/opt/audio/models')
            
            # Try different possible locations for the models.json file
            possible_paths = [
                # First priority: MODELS_CONFIG environment variable
                models_config_path,
                # Second priority: MODELS_DIR + /models.json
                os.path.join(models_dir, "models.json"),
                # Fallback locations
                "/opt/audio/models/models.json",
                "/opt/data/config/models.json",
                "/opt/audio/models/config/models.json",
                # Last resort: repo copy inside the image
                os.path.join(project_root, "engine", "config", "models.json"),
            ]
            
            # Remove duplicates while preserving order
            seen = set()
            unique_paths = []
            for path in possible_paths:
                if path not in seen:
                    seen.add(path)
                    unique_paths.append(path)
            
            for path in unique_paths:
                if os.path.exists(path):
                    logger.info(f"Loading models configuration from {path}")
                    with open(path, 'r') as f:
                        return json.load(f)
            
            logger.warning(f"Models configuration file not found in any of these locations: {unique_paths}")
            logger.warning("Using default configuration")
            return default_config
            
        except Exception as e:
            logger.error(f"Error loading models configuration: {e}", exc_info=True)
            return default_config
    
    @staticmethod
    def get_asr_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Get the ASR models configuration from models.json.
        """
        config = ModelLoader.load_models_config(dev_mode)
        return config.get("asr", {}).get("models", [])
    
    @staticmethod
    def get_translation_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Get the translation models configuration from models.json.
        """
        config = ModelLoader.load_models_config(dev_mode)
        return config.get("translation", {}).get("models", [])
    
    @staticmethod
    def get_tts_models(dev_mode: bool = False) -> List[Dict[str, Any]]:
        """
        Get the TTS models configuration from models.json.
        """
        config = ModelLoader.load_models_config(dev_mode)
        return config.get("tts", {}).get("models", [])
    
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
