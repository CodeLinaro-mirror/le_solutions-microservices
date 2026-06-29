# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
TTS Model Registry

This module provides a generic registry system for TTS model types,
eliminating the need for repetitive conditionals when adding new models.
"""

from typing import Dict, Callable, Any


class ModelConfig:
    """Configuration for a TTS model type."""
    
    def __init__(
        self,
        name: str,
        generate_model_func: Callable,
        generate_packed_file_func: Callable
    ):
        """
        Initialize model configuration.
        
        Args:
            name: Model type name (e.g., "melo", "piper")
            generate_model_func: Function to generate model buffer
            generate_packed_file_func: Function to generate packed model file
        """
        self.name = name
        self.generate_model_func = generate_model_func
        self.generate_packed_file_func = generate_packed_file_func


class TTSModelRegistry:
    """Registry for TTS model types."""
    
    def __init__(self):
        self._models: Dict[str, ModelConfig] = {}
    
    def register(self, model_config: ModelConfig):
        """
        Register a new model type.
        
        Args:
            model_config: ModelConfig instance
        """
        self._models[model_config.name.lower()] = model_config
        print(f"Registered TTS model type: {model_config.name}")
    
    def get(self, model_type: str) -> ModelConfig:
        """
        Get model configuration by type.
        
        Args:
            model_type: Model type name
            
        Returns:
            ModelConfig instance
            
        Raises:
            ValueError: If model type is not registered
        """
        model_type_lower = model_type.lower()
        if model_type_lower not in self._models:
            raise ValueError(
                f"Unknown model type: {model_type}. "
                f"Available types: {list(self._models.keys())}"
            )
        return self._models[model_type_lower]
    
    def is_registered(self, model_type: str) -> bool:
        """
        Check if a model type is registered.
        
        Args:
            model_type: Model type name
            
        Returns:
            True if registered, False otherwise
        """
        return model_type.lower() in self._models
    
    def list_models(self):
        """List all registered model types."""
        return list(self._models.keys())


# Global registry instance
_registry = TTSModelRegistry()


def register_model(model_config: ModelConfig):
    """
    Register a model type in the global registry.
    
    Args:
        model_config: ModelConfig instance
    """
    _registry.register(model_config)


def get_model_config(model_type: str) -> ModelConfig:
    """
    Get model configuration from the global registry.
    
    Args:
        model_type: Model type name
        
    Returns:
        ModelConfig instance
    """
    return _registry.get(model_type)


def is_model_registered(model_type: str) -> bool:
    """
    Check if a model type is registered.
    
    Args:
        model_type: Model type name
        
    Returns:
        True if registered, False otherwise
    """
    return _registry.is_registered(model_type)


def list_registered_models():
    """List all registered model types."""
    return _registry.list_models()


# Register built-in model types
def _register_builtin_models():
    """Register built-in TTS model types."""
    try:
        from melo.tts_melo_model_generation import (
            generate_model as generate_melo_model,
            generate_packed_model_file as generate_melo_packed_file
        )
        
        register_model(ModelConfig(
            name="melo",
            generate_model_func=generate_melo_model,
            generate_packed_file_func=generate_melo_packed_file
        ))
    except ImportError as e:
        print(f"Warning: Could not register Melo model: {e}")
    
    try:
        from piper.tts_piper_model_generation import (
            generate_model as generate_piper_model,
            generate_packed_model_file as generate_piper_packed_file
        )
        
        register_model(ModelConfig(
            name="piper",
            generate_model_func=generate_piper_model,
            generate_packed_file_func=generate_piper_packed_file
        ))
    except ImportError as e:
        print(f"Warning: Could not register Piper model: {e}")


# Auto-register built-in models on module import
_register_builtin_models()
