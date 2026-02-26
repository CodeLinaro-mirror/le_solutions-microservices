# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import logging
import re


class Config:
    """Centralized configuration for Audio Analytics Server"""
    
    # Redis Configuration
    REDIS_HOST = os.environ.get('REDIS_HOST', 'redis')
    REDIS_PORT = int(os.environ.get('REDIS_PORT', 6379))
    
    # Development Mode
    # Check if we're running in development mode by looking at the container name or environment variable
    CONTAINER_NAME = os.environ.get('HOSTNAME', '')
    DEV_MODE = os.environ.get('DEV_MODE', 'false').lower() == 'true' or \
               bool(re.search(r'-dev', CONTAINER_NAME))
    
    # Logging Configuration
    LOG_LEVEL = int(os.environ.get('LOG_LEVEL', logging.INFO))

    T2T_KEEPALIVE_SINGLETON = True
    
    # ASR Channels
    ASR_TRANSCRIPTION_IN = os.environ.get('ASR_TRANSCRIPTION_IN', 'asr.transcription.in')
    ASR_TRANSCRIPTION_OUT = os.environ.get('ASR_TRANSCRIPTION_OUT', 'asr.transcription.out')
    ASR_MODELS = os.environ.get('ASR_MODELS', 'asr.models')
    
    # T2T Channels
    T2T_TRANSLATION_IN = os.environ.get('T2T_TRANSLATION_IN', 't2t.translation.in')
    T2T_TRANSLATION_OUT = os.environ.get('T2T_TRANSLATION_OUT', 't2t.translation.out')
    T2T_MODELS = os.environ.get('T2T_MODELS', 't2t.models')
    
    # TTS Channels
    TTS_TEXT_IN = os.environ.get('TTS_TEXT_IN', 'tts.text.in')
    TTS_AUDIO_OUT = os.environ.get('TTS_AUDIO_OUT', 'tts.audio.out')
    TTS_MODELS = os.environ.get('TTS_MODELS', 'tts.models')
    TTS_DEVICES = os.environ.get('TTS_DEVICES', 'tts.devices')

    # ASR Device Channel
    ASR_DEVICES = os.environ.get('ASR_DEVICES', 'asr.devices')
    
    @classmethod
    def get_all_channels(cls):
        """Get all Redis channels as a dictionary"""
        return {
            'asr': {
                'transcription_in': cls.ASR_TRANSCRIPTION_IN,
                'transcription_out': cls.ASR_TRANSCRIPTION_OUT,
                'models': cls.ASR_MODELS,
            'devices': cls.ASR_DEVICES,
            },
            't2t': {
                'translation_in': cls.T2T_TRANSLATION_IN,
                'translation_out': cls.T2T_TRANSLATION_OUT,
                'models': cls.T2T_MODELS,
            },
            'tts': {
                'text_in': cls.TTS_TEXT_IN,
                'audio_out': cls.TTS_AUDIO_OUT,
                'models': cls.TTS_MODELS,
            'devices': cls.TTS_DEVICES,
            }
        }
    
    @classmethod
    def get_input_channels(cls):
        """Get all input channels that services should subscribe to"""
        return [
            cls.ASR_TRANSCRIPTION_IN,
            cls.T2T_TRANSLATION_IN,
            cls.TTS_TEXT_IN,
            cls.ASR_MODELS,
            cls.ASR_DEVICES,
            cls.T2T_MODELS,
            cls.TTS_MODELS,
            cls.TTS_DEVICES,
        ]
    
    @classmethod
    def get_output_channels(cls):
        """Get all output channels that services publish to"""
        return [
            cls.ASR_TRANSCRIPTION_OUT,
            cls.T2T_TRANSLATION_OUT,
            cls.TTS_AUDIO_OUT,
        ]
