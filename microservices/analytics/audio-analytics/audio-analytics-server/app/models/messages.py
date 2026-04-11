# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""
Message schemas for communication between Audio Analytics API and Server.

These messages are sent over Redis pub/sub channels and define the contract
between the API layer and the processing server.
"""

from dataclasses import dataclass, asdict, field
from typing import Optional, Dict, Any, List, Union
from enum import Enum
import json
import uuid


# ============================================================================
# ASR Messages - API <-> Server Communication
# ============================================================================

@dataclass
class TranscriptionsCreateRequest:
    """
    Message sent from API to Server when /transcriptions/create is called.
    Sent on channel: asr.transcription.in
    """
    message_type: str = "transcriptions_create"
    message_source: str = "audio_analytics_api"
    model: str = "whisper-1"                           # ASR model to use
    language: Optional[str] = None                     # e.g., "en", "es"
    stream: bool = False                               # Streaming output?
    parameters: Optional[Union[str, List[Dict[str, Any]]]] = None  # Custom parameters (e.g., sampling_rate, channels)
    channels: Optional[int] = None                     # Number of audio channels (1=mono, 2=stereo)
    file: Optional[str] = None                         # Base64 encoded audio (or null for live stream)
    filename: Optional[str] = None                     # Original filename
    sync_id: Optional[str] = None                      # For synchronous requests
    keep_alive: Optional[bool] = False                 # Keep ASR engine alive after synthesis (default: False)

    def __post_init__(self):
        if self.sync_id is None and not self.stream:
            self.sync_id = str(uuid.uuid4())
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields to handle legacy/extra client fields
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        # Coerce keep_alive to bool — form fields arrive as strings "true"/"false"
        if 'keep_alive' in filtered_data and not isinstance(filtered_data['keep_alive'], bool):
            filtered_data['keep_alive'] = str(filtered_data['keep_alive']).lower() in ('true', '1', 'yes')
        return cls(**filtered_data)


@dataclass
class TranscriptionsResult:
    """
    Message sent from Server to API with transcription results.
    Sent on channel: asr.transcription.out
    """
    message_type: str = "transcriptions_result"
    stream: bool = False
    result: Dict[str, Any] = field(default_factory=dict)  # Contains: text, language, type, session_id
    sync_id: Optional[str] = None                         # For synchronous responses
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        return cls(**data)
    
    @classmethod
    def create_result(cls, text: str, language: str, result_type: str,
                     stream: bool = False, session_id: Optional[str] = None,
                     sync_id: Optional[str] = None, language_name: Optional[str] = None,
                     state: Optional[str] = None):
        """Helper to create a result message"""
        result = {
            "text": text,
            "language": language,
            "type": result_type  # "transcript.text.done" or "transcript.text.delta"
        }
        if language_name:
            result["language_name"] = language_name
        if session_id:
            result["session_id"] = session_id
        if state:
            result["state"] = state

        return cls(
            message_type="transcriptions_result",
            stream=stream,
            result=result,
            sync_id=sync_id
        )


@dataclass
class TranscriptionsSessionAudio:
    """
    Message sent from API to Server with streaming audio chunks.
    Sent on channel: asr.transcription.in (for live streaming)
    """
    message_type: str = "transcriptions_session_audio"
    message_source: str = "audio_analytics_api"
    session_id: str = ""
    type: str = "input_audio"
    data: str = ""  # Base64 encoded audio chunk
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields (e.g., 'model' from API)
        valid_fields = {'message_type', 'message_source', 'session_id', 'type', 'data'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranscriptionsClose:
    """
    Message sent from API to Server to close transcription session.
    Sent on channel: asr.transcription.in
    """
    message_type: str = "transcriptions_close"
    message_source: str = "audio_analytics_api"
    session_id: Optional[str] = None
    sync_id: Optional[str] = None

    def to_json(self) -> str:
        return json.dumps(asdict(self))

    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields
        valid_fields = {'message_type', 'message_source', 'session_id', 'sync_id'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranscriptionsFlush:
    """
    Message sent from API to Server to flush the audio buffer and force
    immediate processing of any buffered audio.
    Sent on channel: asr.transcription.in
    """
    message_type: str = "transcriptions_flush"
    message_source: str = "audio_analytics_api"
    session_id: Optional[str] = None
    sync_id: Optional[str] = None

    def to_json(self) -> str:
        return json.dumps(asdict(self))

    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {'message_type', 'message_source', 'session_id', 'sync_id'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranscriptionsModelsRequest:
    """
    Message for requesting available ASR models.
    Sent on channel: asr.models
    """
    message_type: str = "models_list"
    message_source: str = "audio_analytics_api"
    sync_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields
        valid_fields = {'message_type', 'message_source', 'sync_id'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranscriptionsModelsResponse:
    """
    Response with list of available ASR models.
    Sent on channel: asr.models
    """
    message_type: str = "models_list_response"
    message_source: str = "audio_analytics_server"
    models: List[str] = field(default_factory=list)  # e.g., ["whisper-1", "whisper-2"]
    sync_id: Optional[str] = None
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


# ============================================================================
# T2T (Translation) Messages - API <-> Server Communication
# ============================================================================

@dataclass
class TranslationRequest:
    """
    Message sent from API to Server for translation.
    Sent on channel: t2t.translation.in
    """
    message_type: str = "translation_request"
    message_source: str = "audio_analytics_api"
    sync_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    text: List[str] = field(default_factory=list)      # Array of texts to translate
    model: str = "translation-model-1"
    source_language: str = "en"                        # e.g., "en", "fr", "es"
    target_language: str = "en"                        # e.g., "en", "fr", "es"
    parameters: Optional[Dict[str, Any]] = None        # e.g., {"formality": "informal"}
    keep_alive: Optional[bool] = False                 # Keep T2T engine alive after translation (default: False)

    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranslationResult:
    """Individual translation result"""
    translated_text: str
    target_language: str
    source_language: str


@dataclass
class TranslationResponse:
    """
    Message sent from Server to API with translation results.
    Sent on channel: t2t.translation.out
    """
    sync_id: str
    message_source: str = "audio_analytics_server"
    result: Dict[str, List[Dict[str, str]]] = field(default_factory=dict)  # {"translations": [...]}
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)
    
    @classmethod
    def create_response(cls, sync_id: str, translations: List[TranslationResult]):
        """Helper to create a translation response"""
        result = {
            "translations": [
                {
                    "translated_text": t.translated_text,
                    "target_language": t.target_language,
                    "source_language": t.source_language
                }
                for t in translations
            ]
        }
        return cls(sync_id=sync_id, result=result)


@dataclass
class TranslationModelsRequest:
    """
    Message for requesting available translation models.
    Sent on channel: t2t.models
    """
    message_type: str = "models_list"
    message_source: str = "audio_analytics_api"
    sync_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TranslationModelsResponse:
    """
    Response with list of available translation models.
    Sent on channel: t2t.models
    """
    message_type: str = "models_list_response"
    message_source: str = "audio_analytics_server"
    models: List[Dict[str, Any]] = field(default_factory=list)
    sync_id: Optional[str] = None
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


# ============================================================================
# TTS (Text-to-Speech) Messages - API <-> Server Communication
# ============================================================================

@dataclass
class TTSSynthesizeRequest:
    """
    Message sent from API to Server for TTS synthesis.
    Sent on channel: tts.text.in
    """
    message_type: str = "tts_synthesize"
    message_source: str = "audio_analytics_api"
    text: str = ""                                     # Text to synthesize
    model: str = "melo-tts-en"
    language: Optional[str] = None                     # e.g., "en", "es"
    voice: Optional[str] = None                        # Voice identifier
    gender: Optional[str] = None                       # "male", "female", "neutral"
    style: Optional[str] = None                        # e.g., "cheerful", "formal"
    parameters: Optional[Dict[str, str]] = None        # e.g., {"speaking_rate": "1.1", "pitch": "0.9"}
    sample_rate: Optional[int] = None                  # Sample rate for output audio
    output_speaker: Optional[bool] = False
    on_device_playback: Optional[bool] = False
    output_speaker_name: Optional[str] = None          # Device name for on-device playback (e.g., "Yeti Nano: USB Audio (hw:0,0)")
    keep_alive: Optional[bool] = False                 # Keep TTS engine alive after synthesis (default: False)
    sync_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    override_max_chars_check: bool = False

    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TTSAudioChunk:
    """
    Message sent from Server to API with audio chunks.
    Sent on channel: tts.audio.out
    
    Note: Audio data is sent as raw binary chunks, not JSON.
    This dataclass is for documentation purposes.
    The actual message is just the binary audio data.
    """
    audio_data: bytes  # Raw audio binary data
    
    # TTS sends raw binary chunks directly, not JSON
    # The final message is a stringified JSON: '{"status": "done"}'


@dataclass
class TTSComplete:
    """
    Final message sent from Server to API when TTS is complete.
    Sent on channel: tts.audio.out
    """
    status: str = "done"
    sync_id: Optional[str] = None
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TTSCloseRequest:
    """
    Message sent from API to Server to close the TTS session and release resources.
    Sent on channel: tts.text.in
    """
    message_type: str = "tts_close"
    message_source: str = "audio_analytics_api"

    def to_json(self) -> str:
        return json.dumps(asdict(self))

    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {'message_type', 'message_source'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TTSCancelRequest:
    """
    Message sent from API to Server to cancel active TTS playback and
    drain any queued synthesis requests.
    Sent on channel: tts.text.in
    """
    message_type: str = "tts_cancel"
    message_source: str = "audio_analytics_api"

    def to_json(self) -> str:
        return json.dumps(asdict(self))

    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {'message_type', 'message_source'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TTSModelsRequest:
    """
    Message for requesting available TTS models.
    Sent on channel: tts.models
    """
    message_type: str = "models_list"
    message_source: str = "audio_analytics_api"
    language: Optional[str] = None  # Optional filter by language
    sync_id: str = field(default_factory=lambda: str(uuid.uuid4()))
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        # Filter out unexpected fields like 'result' from API responses
        valid_fields = {'message_type', 'message_source', 'language', 'sync_id'}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


@dataclass
class TTSModelsResponse:
    """
    Response with list of available TTS models.
    Sent on channel: tts.models
    """
    message_type: str = "models_list_response"
    models: List[Dict[str, Any]] = field(default_factory=list)
    sync_id: Optional[str] = None
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


# ============================================================================
# Error Messages (for any channel)
# ============================================================================

@dataclass
class ErrorMessage:
    """Error message format for any service"""
    error: bool = True
    message: str = ""
    service: Optional[str] = None          # "asr", "t2t", "tts"
    sync_id: Optional[str] = None
    session_id: Optional[str] = None
    
    def to_json(self) -> str:
        return json.dumps(asdict(self))
    
    @classmethod
    def from_json(cls, json_str: str):
        data = json.loads(json_str)
        valid_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered_data = {k: v for k, v in data.items() if k in valid_fields}
        return cls(**filtered_data)


# ============================================================================
# Helper Functions
# ============================================================================

def parse_message(json_str: str) -> Dict[str, Any]:
    """Parse a JSON message and return as dict"""
    return json.loads(json_str)


def get_message_type(json_str: str) -> Optional[str]:
    """Extract message_type from a JSON message"""
    try:
        data = json.loads(json_str)
        return data.get('message_type')
    except:
        return None
