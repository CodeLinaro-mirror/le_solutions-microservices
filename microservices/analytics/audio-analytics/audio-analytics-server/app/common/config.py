# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# Copyright (c) 2022 OpenAI

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

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

    # Models directories
    MODELS_DIR = os.environ.get('MODELS_DIR', '/mnt/work/models/audio')
    ASR_MODELS_DIR = os.environ.get('ASR_MODELS_DIR', os.path.join(MODELS_DIR, 'asr'))
    TRANSLATION_MODELS_DIR = os.environ.get('TRANSLATION_MODELS_DIR', os.path.join(MODELS_DIR, 'translation'))
    TTS_MODELS_DIR = os.environ.get('TTS_MODELS_DIR', os.path.join(MODELS_DIR, 'tts'))

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
    
    # Map language to language code
    MELO_LANGUAGE_CODE_MAP = {
        'en': 0,
        'zh': 1,
        'de': 2,
        'es': 3,
        'ru': 4,
        'ko': 5,
        'fr': 6,
        'ja': 7,
        'pt': 8,
        'tr': 9,
        'pl': 10,
        'ca': 11,
        'nl': 12,
        'ar': 13,
        'sv': 14,
        'it': 15,
        'id': 16,
        'hi': 17,
        'fi': 18,
        'vi': 19,
        'he': 20,
        'uk': 21,
        'el': 22,
        'ms': 23,
        'cs': 24,
        'ro': 25,
        'da': 26,
        'hu': 27,
        'ta': 28,
        'no': 29,
        'th': 30,
        'ur': 31,
        'hr': 32,
        'bg': 33,
        'lt': 34,
        'la': 35,
        'mi': 36,
        'ml': 37,
        'cy': 38,
        'sk': 39,
        'te': 40,
        'fa': 41,
        'lv': 42,
        'bn': 43,
        'sr': 44,
        'az': 45,
        'sl': 46,
        'kn': 47,
        'et': 48,
        'mk': 49,
        'br': 50,
        'eu': 51,
        'is': 52,
        'hy': 53,
        'ne': 54,
        'mn': 55,
        'bs': 56,
        'kk': 57,
        'sq': 58,
        'sw': 59,
        'gl': 60,
        'mr': 61,
        'pa': 62,
        'si': 63,
        'km': 64,
        'sn': 65,
        'yo': 66,
        'so': 67,
        'af': 68,
        'oc': 69,
        'ka': 70,
        'be': 71,
        'tg': 72,
        'sd': 73,
        'gu': 74,
        'am': 75,
        'yi': 76,
        'lo': 77,
        'uz': 78,
        'fo': 79,
        'ht': 80,
        'ps': 81,
        'tk': 82,
        'nn': 83,
        'mt': 84,
        'sa': 85,
        'lb': 86,
        'my': 87,
        'bo': 88,
        'tl': 89,
        'mg': 90,
        'as': 91,
        'tt': 92,
        'haw': 93,
        'ln': 94,
        'ha': 95,
        'ba': 96,
        'jw': 97,
        'su': 98
    }

    PIPER_LANGUAGE_CODE_MAP = {
        'en': 0,
        'zh': 1,
        'de': 2,
        'es': 3,
        'ru': 4,
        'ko': 5,
        'fr': 6,
        'ja': 7,
        'pt': 8,
        'tr': 9,
        'pl': 10,
        'ca': 11,
        'nl': 12,
        'ar': 13,
        'sv': 14,
        'it': 15,
        'id': 16,
        'hi': 17,
        'fi': 18,
        'vi': 19,
        'he': 20,
        'uk': 21,
        'el': 22,
        'ms': 23,
        'cs': 24,
        'ro': 25,
        'da': 26,
        'hu': 27,
        'ta': 28,
        'no': 29,
        'th': 30,
        'ur': 31,
        'hr': 32,
        'bg': 33,
        'lt': 34,
        'la': 35,
        'mi': 36,
        'ml': 37,
        'cy': 38,
        'sk': 39,
        'te': 40,
        'fa': 41,
        'lv': 42,
        'bn': 43,
        'sr': 44,
        'az': 45,
        'sl': 46,
        'kn': 47,
        'et': 48,
        'mk': 49,
        'br': 50,
        'eu': 51,
        'is': 52,
        'hy': 53,
        'ne': 54,
        'mn': 55,
        'bs': 56,
        'kk': 57,
        'sq': 58,
        'sw': 59,
        'gl': 60,
        'mr': 61,
        'pa': 62,
        'si': 63,
        'km': 64,
        'sn': 65,
        'yo': 66,
        'so': 67,
        'af': 68,
        'oc': 69,
        'ka': 70,
        'be': 71,
        'tg': 72,
        'sd': 73,
        'gu': 74,
        'am': 75,
        'yi': 76,
        'lo': 77,
        'uz': 78,
        'fo': 79,
        'ht': 80,
        'ps': 81,
        'tk': 82,
        'nn': 83,
        'mt': 84,
        'sa': 85,
        'lb': 86,
        'my': 87,
        'bo': 88,
        'tl': 89,
        'mg': 90,
        'as': 91,
        'tt': 92,
        'haw': 93,
        'ln': 94,
        'ha': 95,
        'ba': 96,
        'jw': 97,
        'su': 98
    }

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
