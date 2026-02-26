# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import Dict, Callable, Optional, List, Any
import time
import asyncio
import os
import sys
import importlib.util
import tempfile
import wave
import io
import struct
import ctypes
import json
from ctypes import CFUNCTYPE, POINTER, c_char_p, c_int32, c_void_p, Structure
from common.base_service import BaseService
from common.redis_client import RedisClient
from common.config import Config
from common.utils import base64_to_bytes
from common.logger import get_logger
from common.model_loader import ModelLoader
from common.resampler import AudioResampler, resample_audio
from common.service_coordinator import get_service_coordinator
from models.messages import (
    TranscriptionsCreateRequest,
    TranscriptionsResult,
    TranscriptionsSessionAudio,
    TranscriptionsClose,
    TranscriptionsModelsRequest,
    TranscriptionsModelsResponse,
    get_message_type
)

import threading
from utils import Recorder

logger = get_logger(__name__)


# Define the C structure for transcription key-value pairs
class WhisperTranscriptionKV(Structure):
    _fields_ = [
        ("key", c_char_p),
        ("value", c_char_p),
    ]


# Define the callback type for transcription results
TranscriptionCallback = CFUNCTYPE(
    None,
    POINTER(WhisperTranscriptionKV),  # const WhisperTranscriptionKV* results
    c_int32,                          # int count
    c_void_p                          # void* user_data
)

FINALIZATION_GRACE_SEC = 10

class ASRService(BaseService):
    """
    Automatic Speech Recognition (ASR) Service.
    Handles transcription requests from audio files and live streams.
    """
    
    def __init__(self, redis_client: RedisClient):
        super().__init__(redis_client, "ASR")
        self.active_sessions: Dict[str, Dict] = {}
        self.dev_mode = Config.DEV_MODE
        self.model_config = ModelLoader.get_asr_models(self.dev_mode)
        self.available_models = [model["name"] for model in self.model_config]
        self.vad_len_hangover = self._get_vad_len_hangover_from_env()
        
        self.speakers = dict()

                # Singleton WhisperWrapper instance (reused across all requests)
        self.whisper_wrapper = None
        self.whisper_wrapper_lock = asyncio.Lock()

        # Singleton Recorder instance (reused across sessions)
        self.recorder: Optional[Recorder] = None
        self.recorder_thread: Optional[threading.Thread] = None
        self.recorder_ready = asyncio.Event()
        # Actual sample rate the recorder is opened at (may differ from 16000 when the
        # hardware device does not support 16 kHz natively).  Audio is resampled to
        # 16000 Hz in handle_recording_audio before being passed to the ASR engine.
        self.recorder_sample_rate: int = 16000
        # Timestamp of the last audio chunk processed — used to detect idle timeout.
        # If the recorder has not been used for RECORDER_IDLE_TIMEOUT_SEC seconds the
        # singleton is considered stale and will be recreated on next use.
        self.recorder_last_used: float = 0.0
        self.RECORDER_IDLE_TIMEOUT_SEC: int = 5 * 60  # 5 minutes
        
        # Service coordinator for managing resource conflicts with T2T
        self.coordinator = get_service_coordinator()

        # Initialize ASR engine if not in dev mode
        self.asr_engine = None
        if not self.dev_mode:
            self.logger.info("Running in production mode - initializing ASR engine")
            try:
                # Import the ASR wrapper module (now in server directory)
                wrapper_path = os.path.join(
                    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "asr_wrapper.py"
                )
                if os.path.exists(wrapper_path):
                    self.logger.info(f"Found ASR wrapper at {wrapper_path}")
                    # Import the module dynamically
                    spec = importlib.util.spec_from_file_location("asr_wrapper", wrapper_path)
                    if spec and spec.loader:
                        asr_wrapper_module = importlib.util.module_from_spec(spec)
                        sys.modules["asr_wrapper"] = asr_wrapper_module
                        spec.loader.exec_module(asr_wrapper_module)
                        self.asr_engine = asr_wrapper_module
                        self.logger.info("ASR wrapper module loaded successfully")
                    else:
                        self.logger.error("Failed to load ASR wrapper module specification")
                else:
                    self.logger.warning(f"ASR wrapper not found at {wrapper_path}")
            except Exception as e:
                self.logger.error(f"Error initializing ASR wrapper: {e}", exc_info=True)
        else:
            self.logger.info("Running in development mode - using mock responses")
    
    def _get_vad_len_hangover_from_env(self) -> int:
        """
        Read VAD_LEN_HANGOVER from environment variable.
        Default value is 70 (in units of 10ms, so 70 = 700ms = 0.7 seconds).
        
        Note: The environment value is in units of 10 milliseconds.
        For example: 120 = 1200ms = 1.2 seconds
        
        Returns:
            VAD length hangover value in milliseconds (for wrapper to convert to 10ms units)
        """
        try:
            vad_len_hangover_10ms = int(os.environ.get('VAD_LEN_HANGOVER', '70'))
            vad_len_hangover_ms = vad_len_hangover_10ms * 10  # Convert to milliseconds for wrapper
            self.logger.info(f"VAD_LEN_HANGOVER set to {vad_len_hangover_10ms} (={vad_len_hangover_ms}ms = {vad_len_hangover_ms / 1000}s) from environment")
            return vad_len_hangover_ms
        except ValueError:
            self.logger.warning(f"Invalid VAD_LEN_HANGOVER value in environment, using default 700ms (0.7s)")
            return 700  # 70 * 10ms = 700ms
        
    def get_subscriptions(self) -> Dict[str, Callable]:
        """Subscribe to ASR input channels."""
        return {
            Config.ASR_TRANSCRIPTION_IN: self.handle_message_safely(self.handle_transcription_input),
            Config.ASR_MODELS: self.handle_message_safely(self.handle_models_request),
            Config.ASR_DEVICES: self.handle_message_safely(self.handle_devices_request),
        }
    
    async def initialize(self):
        """Initialize ASR engine and load models."""
        self.logger.info('Initializing ASR service...')
        
        if not self.dev_mode and self.asr_engine:
            try:
                # Initialize the ASR engine
                self.logger.info("Initializing ASR engine...")
                # The WhisperWrapper will be created per-session or per-request
                # No global initialization needed here
                self.logger.info("ASR engine ready")
                
                # Log available models from config
                self.logger.info(f"Available ASR models: {self.available_models}")
            except Exception as e:
                self.logger.error(f"Error initializing ASR engine: {e}", exc_info=True)
        else:
            self.logger.info(f'Using mock models: {self.available_models}')
    
    async def detect_audio_format(self, audio_bytes):
        """Detect the format of audio data based on header bytes."""
        # Check for WAV header (RIFF....WAVE)
        if len(audio_bytes) > 12 and audio_bytes[0:4] == b'RIFF' and audio_bytes[8:12] == b'WAVE':
            self.logger.info("Detected WAV format")
            return "wav"
        
        # If no WAV header is detected, assume it's raw PCM data
        self.logger.info("No WAV header detected, assuming raw PCM data")
        return "raw"
    
    async def extract_wav_data(self, audio_bytes):
        """Extract audio data from WAV file, skipping the header."""
        try:
            # Check if it's a valid WAV file
            if len(audio_bytes) <= 44 or audio_bytes[0:4] != b'RIFF' or audio_bytes[8:12] != b'WAVE':
                self.logger.warning("Not a valid WAV file")
                return None, None, None, None
            
            # Parse WAV header
            # Format chunk should start at byte 12 with 'fmt '
            fmt_pos = audio_bytes.find(b'fmt ', 12)
            if fmt_pos < 0:
                self.logger.warning("Could not find format chunk in WAV file")
                return None, None, None, None
            
            # Read format chunk
            # Format chunk structure: 'fmt ' (4 bytes), chunk size (4 bytes), format data...
            fmt_size = struct.unpack('<I', audio_bytes[fmt_pos+4:fmt_pos+8])[0]
            fmt_data = audio_bytes[fmt_pos+8:fmt_pos+8+fmt_size]
            
            # Parse format data
            # Basic PCM format: audio format (2 bytes), channels (2 bytes), sample rate (4 bytes),
            # byte rate (4 bytes), block align (2 bytes), bits per sample (2 bytes)
            if len(fmt_data) >= 16:
                audio_format, channels, sample_rate, _, _, bits_per_sample = struct.unpack('<HHIIHH', fmt_data[:16])
                self.logger.info(f"WAV format: {audio_format}, channels: {channels}, sample rate: {sample_rate}, bits: {bits_per_sample}")
            else:
                self.logger.warning("Format chunk too small")
                return None, None, None, None
            
            # Find data chunk
            data_pos = audio_bytes.find(b'data', fmt_pos + 8 + fmt_size)
            if data_pos < 0:
                self.logger.warning("Could not find data chunk in WAV file")
                return None, None, None, None
            
            # Get data size and actual audio data
            data_size = struct.unpack('<I', audio_bytes[data_pos+4:data_pos+8])[0]
            audio_data = audio_bytes[data_pos+8:data_pos+8+data_size]
            
            self.logger.info(f"Extracted {len(audio_data)} bytes of audio data from WAV file")
            return audio_data, channels, sample_rate, bits_per_sample
            
        except Exception as e:
            self.logger.error(f"Error extracting WAV data: {e}", exc_info=True)
            return None, None, None, None
    
    async def prepare_audio_file(self, audio_bytes, target_sample_rate=16000, target_channels=1):
        """Prepare audio file for processing by detecting format and converting if needed."""
        try:
            # Detect audio format
            audio_format = await self.detect_audio_format(audio_bytes)
            
            # Create a temporary file for the output WAV
            with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as temp_output_file:
                temp_output_path = temp_output_file.name
            
            if audio_format == "wav":
                # For WAV files, check if we need to extract the audio data
                audio_data, channels, sample_rate, bits_per_sample = await self.extract_wav_data(audio_bytes)
                
                if audio_data is not None:
                    # If the WAV file has the right format (16kHz, 16-bit, mono), we can use it directly
                    if channels == target_channels and bits_per_sample == 16 and sample_rate == target_sample_rate:
                        self.logger.info(f"WAV file already in correct format ({target_sample_rate}Hz, 16-bit, {target_channels} channel(s))")
                        # Write the audio data to the temporary file
                        with wave.open(temp_output_path, 'wb') as wav_file:
                            wav_file.setnchannels(target_channels)
                            wav_file.setsampwidth(2)  # 16-bit
                            wav_file.setframerate(target_sample_rate)
                            wav_file.writeframes(audio_data)
                        return temp_output_path
                    else:
                        self.logger.info(f"WAV file needs conversion: channels={channels}, bits={bits_per_sample}, rate={sample_rate}")
                        # Use our resampler to convert the audio
                        try:
                            # Resample the WAV file
                            resampled_data = resample_audio(
                                audio_bytes,
                                original_sample_rate=sample_rate,
                                target_sample_rate=target_sample_rate,
                                original_channels=channels,
                                target_channels=target_channels,
                                is_wav_file=True
                            )
                            
                            # Write the resampled data to the temporary file
                            with open(temp_output_path, 'wb') as f:
                                f.write(resampled_data)
                            
                            self.logger.info(f"Successfully resampled WAV file to {target_sample_rate}Hz, {target_channels} channel(s)")
                            return temp_output_path
                        except Exception as e:
                            self.logger.error(f"Error resampling WAV file: {e}", exc_info=True)
                            # Fall back to simple conversion
                            with wave.open(temp_output_path, 'wb') as wav_file:
                                wav_file.setnchannels(target_channels)
                                wav_file.setsampwidth(2)  # 16-bit
                                wav_file.setframerate(target_sample_rate)
                                wav_file.writeframes(audio_data)
                            return temp_output_path
                else:
                    # If we couldn't extract the audio data, try to resample the whole file
                    self.logger.warning("Could not extract audio data from WAV file, attempting to resample entire file")
                    try:
                        # Resample the WAV file
                        resampled_data = resample_audio(
                            audio_bytes,
                            original_sample_rate=44100,  # Assume common sample rate if unknown
                            target_sample_rate=target_sample_rate,
                            original_channels=2,  # Assume stereo if unknown
                            target_channels=target_channels,
                            is_wav_file=True
                        )
                        
                        # Write the resampled data to the temporary file
                        with open(temp_output_path, 'wb') as f:
                            f.write(resampled_data)
                        
                        self.logger.info(f"Successfully resampled WAV file to {target_sample_rate}Hz, {target_channels} channel(s)")
                        return temp_output_path
                    except Exception as e:
                        self.logger.error(f"Error resampling WAV file: {e}", exc_info=True)
                        # Fall back to writing the original file
                        with open(temp_output_path, 'wb') as f:
                            f.write(audio_bytes)
                        return temp_output_path
            
            else:  # raw or other format
                # For raw PCM data or unknown formats, create a basic WAV file
                self.logger.info(f"Creating WAV file from {audio_format} data")
                with wave.open(temp_output_path, 'wb') as wav_file:
                    wav_file.setnchannels(target_channels)
                    wav_file.setsampwidth(2)  # 16-bit
                    wav_file.setframerate(target_sample_rate)
                    wav_file.writeframes(audio_bytes)
                
                return temp_output_path
        
        except Exception as e:
            self.logger.error(f"Error preparing audio file: {e}", exc_info=True)
            return None
    
    async def _cleanup_all_sessions(self):
        """Clean up all active sessions. Only one session can be active at a time."""
        if not self.active_sessions:
            return
            
        self.logger.info(f'Cleaning up {len(self.active_sessions)} active session(s) before starting new request...')
        
        # Stop and reset the wrapper if it exists
        if not self.dev_mode and self.whisper_wrapper is not None:
            try:
                self.logger.info('Stopping and resetting wrapper for session cleanup')
                await asyncio.to_thread(self.whisper_wrapper.stop_and_reset)
            except Exception as e:
                self.logger.error(f"Error stopping wrapper during cleanup: {e}", exc_info=True)
        
        # Clear the session data - don't close the singleton wrapper
        self.active_sessions.clear()
        self.logger.info('All sessions cleaned up (wrapper kept alive as singleton)')
    
    async def cleanup_resources_for_service_switch(self):
        """Cleanup ASR resources when switching to another service."""
        self.logger.info('Cleaning up ASR resources for service switch...')
        await self._cleanup_all_sessions()
        

        # Clean up the singleton wrapper to free resources for T2T
        if self.whisper_wrapper is not None:
            self.logger.info("Releasing WhisperWrapper resources for T2T...")
            try:
                # First stop any ongoing processing
                try:
                    await asyncio.wait_for(
                        asyncio.to_thread(self.whisper_wrapper.stop),
                        timeout=0.1
                    )
                    self.logger.info("WhisperWrapper processing stopped")
                except asyncio.TimeoutError:
                    self.logger.warning("Timeout stopping WhisperWrapper")
                except Exception as e:
                    self.logger.error(f"Error stopping WhisperWrapper: {e}")
                
                # Then close the wrapper to release all resources
                try:
                    await asyncio.wait_for(
                        asyncio.to_thread(self.whisper_wrapper.close),
                        timeout=5.0
                    )
                    self.logger.info("WhisperWrapper closed and resources released")
                    await asyncio.sleep(0.25)
                except asyncio.TimeoutError:
                    self.logger.warning("Timeout closing WhisperWrapper")
                except Exception as e:
                    self.logger.error(f"Error closing WhisperWrapper: {e}")
                
                # Clear the wrapper reference
                self.whisper_wrapper = None
                
                # Force garbage collection
                # import gc
                # gc.collect()
                self.logger.info("ASR resources released for service switch")
            except Exception as e:
                self.logger.error(f"Error during ASR resource cleanup: {e}", exc_info=True)
    
    async def cleanup(self):
        """Cleanup ASR resources."""
        self.logger.info('Cleaning up ASR service...')
        await self._cleanup_all_sessions()
        
        # Clean up the singleton wrapper on service shutdown
        if self.whisper_wrapper is not None:
            self.logger.info("Cleaning up singleton WhisperWrapper...")
            try:
                # First stop any ongoing processing
                self.logger.info("Stopping WhisperWrapper processing...")
                try:
                    await asyncio.wait_for(
                        asyncio.to_thread(self.whisper_wrapper.stop),
                        timeout=2.0
                    )
                    self.logger.info("WhisperWrapper processing stopped successfully")
                except asyncio.TimeoutError:
                    self.logger.warning("Timeout stopping WhisperWrapper processing")
                except Exception as e:
                    self.logger.error(f"Error stopping WhisperWrapper processing: {e}")
                
                # Then close the wrapper to release all resources
                self.logger.info("Closing WhisperWrapper to release all resources...")
                try:
                    await asyncio.wait_for(
                        asyncio.to_thread(self.whisper_wrapper.close),
                        timeout=5.0
                    )
                    self.logger.info("WhisperWrapper closed successfully")
                except asyncio.TimeoutError:
                    self.logger.warning("Timeout closing WhisperWrapper")
                except Exception as e:
                    self.logger.error(f"Error closing WhisperWrapper: {e}")
                
                # Force garbage collection to clean up any lingering references
                try:
                    # import gc
                    # gc.collect()
                    self.logger.info("Forced garbage collection to clean up resources")
                except Exception as e:
                    self.logger.error(f"Error during garbage collection: {e}")
                
                self.whisper_wrapper = None
            except Exception as e:
                self.logger.error(f"Error cleaning up singleton wrapper: {e}", exc_info=True)
        
        # Clean up ASR engine if not in dev mode
        if not self.dev_mode and self.asr_engine:
            self.logger.info("Cleaning up ASR engine resources")
            
            # Force garbage collection again to ensure all resources are released
            try:
                # import gc
                # gc.collect()
                self.logger.info("Final garbage collection to clean up ASR engine resources")
            except Exception as e:
                self.logger.error(f"Error during final garbage collection: {e}")
    
    def handle_transcription_input(self, message: str):
        """
        Handle incoming transcription requests.
        Routes to appropriate handler based on message type.
        """
        try:
            # Try to parse as JSON first
            try:
                import json
                data = json.loads(message)
                msg_type = data.get('message_type')
                
                self.logger.debug(f'Received message: type={data.get("type")}, message_type={msg_type}, session_id={data.get("session_id")}, keys={list(data.keys())}')
                
                # Log first 100 chars of message for debugging
                self.logger.debug(f'Raw message: {message[:100]}...')
                
                # No special control messages needed - session already created via HTTP endpoint
                # Just ignore any legacy control messages
                if data.get('type') in ['session.start', 'audio.start', 'audio.end']:
                    self.logger.debug(f"Ignoring legacy control message: {data.get('type')}")
                    return
            except (json.JSONDecodeError, TypeError) as e:
                self.logger.warning(f'Failed to parse message as JSON: {e}')
                self.logger.debug(f'Raw message that failed parsing: {message[:200]}...')
                msg_type = None
            
            if msg_type == "transcriptions_create":
                asyncio.create_task(self.handle_create_transcription(message))
            elif msg_type == "transcriptions_session_audio":
                asyncio.create_task(self.handle_session_audio(message))
            elif msg_type == "transcriptions_close":
                asyncio.create_task(self.handle_close_transcription(message))
            else:
                self.logger.warning(f'Unknown message type: {msg_type}')
                
        except Exception as e:
            self.logger.error(f'Error handling transcription input: {e}', exc_info=True)
    
    async def handle_create_transcription(self, message: str):
        """
        Handle transcription creation request.
        Supports file-based (sync/async) and live streaming.
        """
        try:
            # Request service start - this will cleanup T2T if it's active
                        # Parse the request first so keep_alive is set before request_service_start
            # runs — the cleanup callback reads keep_alive, so it must be current
            request = TranscriptionsCreateRequest.from_json(message)
            self.coordinator.set_asr_keep_alive(request.keep_alive)

            await self.coordinator.request_service_start(
                'ASR',
                self.cleanup_resources_for_service_switch
            )
            self.logger.info(
                f'Transcription request: model={request.model}, '
                f'stream={request.stream}, has_file={request.file is not None}, '
                f'filename={request.filename}, language={request.language}, keep_alive={request.keep_alive}')

            # Extract optional parameters
            sampling_rate = 16000  # Default to 16kHz
            channels = 1  # Default to mono
            vad_value = None  # VAD parameter from client
            on_device_recording = False  # On-device recording flag
            
            if hasattr(request, 'parameters') and request.parameters:
                # Parse parameters
                params = []
                if isinstance(request.parameters, str):
                    try:
                        # Try to parse as JSON string
                        params = json.loads(request.parameters)
                    except json.JSONDecodeError:
                        self.logger.warning(f"Could not parse parameters as JSON: {request.parameters}")
                elif isinstance(request.parameters, list):
                    params = request.parameters
                elif isinstance(request.parameters, dict):
                    # Convert dict to list of key-value pairs
                    params = [{"key": k, "value": v} for k, v in request.parameters.items()]
                
                # Extract sampling_rate, channels, vad, and on_device_recording
                for param in params:
                    if isinstance(param, dict):
                        if param.get("key") == "sampling_rate" and param.get("value"):
                            try:
                                sampling_rate = int(param["value"])
                                self.logger.info(f"Using custom sampling rate: {sampling_rate} Hz")
                            except (ValueError, TypeError):
                                self.logger.warning(f"Invalid sampling_rate value: {param['value']}")
                        
                        elif param.get("key") == "channels" and param.get("value"):
                            try:
                                channels = int(param["value"])
                            except (ValueError, TypeError):
                                self.logger.warning(f"Invalid channels value: {param['value']}")
                        elif param.get("key") == "vad" and param.get("value"):
                            try:
                                # Ensure we convert to int, even if it comes as float
                                vad_value = int(float(param["value"]))
                                self.logger.info(f"Using custom VAD value: {vad_value} (units of 10ms)")
                            except (ValueError, TypeError):
                                self.logger.warning(f"Invalid vad value: {param['value']}")
                        
                        elif param.get("key") == "on_device_recording" and param.get("value"):
                            on_device_recording = param["value"].lower() in ["true", "1", "yes"]
                            self.logger.info(f"On-device recording: {on_device_recording}")
            
            # Validate file format if file is provided
            if request.file and request.filename:
                filename_lower = request.filename.lower()
                if not filename_lower.endswith('.wav'):
                    await self.send_error(
                        Config.ASR_TRANSCRIPTION_OUT,
                        f'Unsupported file format for "{request.filename}". Only .wav files are supported.',
                        sync_id=request.sync_id,
                        param='file'
                    )
                    return
            
            # Validate model
            if request.model not in self.available_models:
                # If model not specified or not found, use default model
                if not request.model:
                    # Find default model from config
                    default_model = next((model["name"] for model in self.model_config 
                                        if model.get("default", False)), None)
                    if default_model:
                        self.logger.info(f"Using default model: {default_model}")
                        request.model = default_model
                    elif self.available_models:
                        # Use first available model
                        request.model = self.available_models[0]
                        self.logger.info(f"Using first available model: {request.model}")
                    else:
                        await self.send_error(
                            Config.ASR_TRANSCRIPTION_OUT,
                            f'No ASR models available on this server.',
                            sync_id=request.sync_id,
                            param='model'
                        )
                        return
                else:
                    await self.send_error(
                        Config.ASR_TRANSCRIPTION_OUT,
                        f'ASR model "{request.model}" is not available. Available models: {", ".join(self.available_models)}',
                        sync_id=request.sync_id,
                        param='model'
                    )
                    return
            
            # Store audio parameters in request for later use
            request.sampling_rate = sampling_rate
            request.channels = channels
            request.vad_value = vad_value
            request.on_device_recording = on_device_recording
            
            if request.file:
                # File-based transcription
                await self.process_file_transcription(request)
            else:
                # Live streaming - create session
                await self.create_streaming_session(request)
                
        except Exception as e:
            self.logger.error(f'Error creating transcription: {e}', exc_info=True)
            await self.send_error(
                Config.ASR_TRANSCRIPTION_OUT,
                str(e),
                sync_id=getattr(request, 'sync_id', None) if 'request' in locals() else None
            )
    
    async def extract_raw_pcm_from_file(self, audio_bytes: bytes) -> tuple:
        """
        Extract raw PCM data from audio file, stripping WAV headers.
        
        Returns:
            tuple: (raw_pcm_data, sample_rate, channels, bits_per_sample)
        """
        # Detect format
        audio_format = await self.detect_audio_format(audio_bytes)
        
        if audio_format == "wav":
            # Extract WAV data
            pcm_data, channels, sample_rate, bits_per_sample = await self.extract_wav_data(audio_bytes)
            if pcm_data is not None:
                self.logger.info(f"Extracted {len(pcm_data)} bytes of raw PCM from WAV (rate={sample_rate}Hz, channels={channels}, bits={bits_per_sample})")
                return pcm_data, sample_rate, channels, bits_per_sample
            else:
                self.logger.warning("Failed to extract WAV data, treating as raw PCM")
                return audio_bytes, 16000, 1, 16
        else:
            # Already raw PCM
            self.logger.info(f"Audio is already raw PCM format ({len(audio_bytes)} bytes)")
            return audio_bytes, 16000, 1, 16
    
    async def process_file_transcription(self, request: TranscriptionsCreateRequest):
        """
        Process file-based transcription (with or without streaming output).
        
        For streaming=false: Concatenates all partial results and returns final complete transcription.
        For streaming=true: Sends partial results as they arrive.
        """
        try:
            # Clean up any existing sessions first (only one session allowed at a time)
            await self._cleanup_all_sessions()
            
            # Decode audio from base64
            audio_bytes = base64_to_bytes(request.file)
            if not audio_bytes:
                raise ValueError("Failed to decode audio file")
            
            # Get sampling rate and channels from request
            sampling_rate = getattr(request, 'sampling_rate', 16000)
            channels = getattr(request, 'channels', 1)
            
            self.logger.info(
                f'Processing audio file: {len(audio_bytes)} bytes, '
                f'streaming={request.stream}, '
                f'sampling_rate={sampling_rate}, '
                f'channels={channels}'
            )
            
            if self.dev_mode:
                # In development mode, use mock responses
                self.logger.info("Using mock transcription in development mode")
                await asyncio.sleep(0.5)  # Simulate processing
                
                if request.stream:
                    # Send streaming results
                    await self.send_streaming_results(request)
                else:
                    # Send single concatenated result
                    await self.send_final_result(request)
            else:
                # In production mode, use the actual ASR engine
                self.logger.info("Using actual ASR engine for transcription")
                
                if self.asr_engine:
                    try:
                        # Get model configuration
                        model_info = next((m for m in self.model_config if m["name"] == request.model), None)
                        if not model_info:
                            raise ValueError(f"Model configuration not found for {request.model}")
                        
                        # Extract model paths from config
                        encoder_path = model_info.get("model_path", "").encode("utf-8")
                        decoder_path = model_info.get("decoder_path", "").encode("utf-8")
                        vocab_path = model_info.get("vocab_path", "").encode("utf-8")
                        speech_path = model_info.get("speech_path", "").encode("utf-8")
                        model_path = b""  # Empty for now
                        
                        self.logger.info(f"Using model paths: encoder={encoder_path}, decoder={decoder_path}, vocab={vocab_path}, speech={speech_path}")
                        
                        # Extract raw PCM data from the file (strip headers)
                        raw_pcm, detected_rate, detected_channels, bits_per_sample = await self.extract_raw_pcm_from_file(audio_bytes)
                        
                        self.logger.info(
                            f"Extracted raw PCM: {len(raw_pcm)} bytes, "
                            f"detected rate={detected_rate}Hz, channels={detected_channels}, bits={bits_per_sample}"
                        )
                        
                        # Resample if needed to match requested format
                        if detected_rate != sampling_rate or detected_channels != channels:
                            self.logger.info(
                                f"Resampling from {detected_rate}Hz {detected_channels}ch "
                                f"to {sampling_rate}Hz {channels}ch"
                            )
                            try:
                                raw_pcm = resample_audio(
                                    raw_pcm,
                                    original_sample_rate=detected_rate,
                                    target_sample_rate=sampling_rate,
                                    original_channels=detected_channels,
                                    target_channels=channels,
                                    sample_width=bits_per_sample // 8,
                                    is_wav_file=False  # Already raw PCM
                                )
                                self.logger.info(f"Resampled to {len(raw_pcm)} bytes")
                            except Exception as e:
                                self.logger.error(f"Error resampling: {e}", exc_info=True)
                        
                        # Set up callbacks to capture transcription results
                        transcription_results = []
                        processing_complete = asyncio.Event()
                        speech_ended_count = 0
                        
                        # Get the event loop to schedule coroutines from C callback thread
                        loop = asyncio.get_event_loop()
                        
                        def capture_transcription(results, count, user_data):
                            # Parse the key-value pairs from the C callback
                            result_dict = {}
                            for i in range(count):
                                kv = results[i]
                                key = kv.key.decode("utf-8") if kv.key else ""
                                value = kv.value.decode("utf-8") if kv.value else ""
                                result_dict[key] = value
                            
                            # Extract the transcription text and is_final flag
                            transcription_text = result_dict.get("transcription", "")
                            is_final_str = result_dict.get("is_final", "true")  # Note: key is "is_final" not "isFinal"
                            # Handle various possible values: "true", "True", "1", "false", "False", "0"
                            is_final = is_final_str.lower() in ["true", "1"]
                            
                            self.logger.info(f"Received transcription chunk: is_final={is_final} (raw='{is_final_str}'), text={transcription_text[:100]}...")
                            
                            if transcription_text:
                                transcription_results.append(transcription_text)
                                
                                # Send results immediately to avoid lag
                                if request.stream:
                                    # For streaming mode, send each chunk as it arrives
                                    # Check isFinal to determine result type
                                    result_type = "transcript.text.done" if is_final else "transcript.text.delta"
                                
                                    # Use call_soon_threadsafe since callback is from C thread
                                    loop.call_soon_threadsafe(
                                        asyncio.create_task,
                                        self._send_streaming_chunk_with_type(
                                            transcription_text,
                                            request.language or "en",
                                            request.sync_id,
                                            result_type
                                        )
                                    )
                                else:
                                    # For non-streaming mode, collect results
                                    # The final result will be the concatenation
                                    pass  # Will concatenate at the end
                        
                        # Define event callback type
                        EventCallback = CFUNCTYPE(None, c_int32, c_void_p)

                        def capture_event(event_code, user_data):
                            """Callback to receive ASR events."""
                            nonlocal speech_ended_count
                            try:
                                # Get event constants from wrapper
                                if event_code == wrapper.EVENT_SPEECH_STARTED:
                                    self.logger.info(f"[ASR Event] Speech started")
                                elif event_code == wrapper.EVENT_SPEECH_ENDED:
                                    speech_ended_count += 1
                                    self.logger.info(f"[ASR Event] Speech ended (count: {speech_ended_count})")
                                    # Signal that we might be done processing
                                    # Set event after a short delay to allow final transcription callbacks
                                    # def signal_complete():
                                    #     loop.call_soon_threadsafe(processing_complete.set)
                                    # import threading
                                    # threading.Timer(0.5, signal_complete).start()
                                else:
                                    self.logger.info(f"[ASR Event] Unknown event code: {event_code}")
                            except Exception as e:
                                self.logger.error(f"Error in event callback: {e}", exc_info=True)


                        # Get or create singleton WhisperWrapper instance
                        async with self.whisper_wrapper_lock:
                            if self.whisper_wrapper is None:
                                self.logger.info("Creating singleton WhisperWrapper instance...")
                                language = request.language.encode("utf-8") if request.language else None
                                
                                # Determine continuous mode based on VAD parameter
                                vad_value = getattr(request, 'vad_value', None)
                                if vad_value is not None:
                                    # If VAD is provided, use non-continuous mode with custom VAD
                                    continuous_mode = False
                                    vad_hangover = vad_value
                                    self.logger.info(f"Using VAD mode: continuous=False, vad_hangover={vad_hangover}")
                                else:
                                    # Default behavior: continuous mode with environment VAD
                                    continuous_mode = True
                                    vad_hangover = self.vad_len_hangover
                                    self.logger.info(f"Using continuous mode: continuous=True, vad_hangover={vad_hangover}")
                                
                                logger.info(f"asr-service process_file_transcription language={language}, continuous={continuous_mode}, partial_transcriptions={request.stream}")
                                
                                self.whisper_wrapper = self.asr_engine.WhisperWrapper(
                                    language=language,
                                    translation_enabled=False,
                                    continuous=continuous_mode,
                                    partial_transcriptions=request.stream,
                                    on_transcription=TranscriptionCallback(capture_transcription),
                                    on_event=EventCallback(capture_event),
                                    on_error=None
                                )
                                
                                # Initialize Whisper with model paths
                                await asyncio.to_thread(self.whisper_wrapper._init_whisper,
                                    encoder_path=encoder_path,
                                    decoder_path=decoder_path,
                                    vocab_path=vocab_path,
                                    speech_path=speech_path,
                                    model_path=model_path
                                )
                                
                                # Set VAD length hangover (custom or environment value)
                                self.whisper_wrapper.set_vad_len_hangover(vad_hangover)

                                self.logger.info("Singleton WhisperWrapper created and initialized")
                            else:
                                self.logger.info("Reusing existing singleton WhisperWrapper instance")
                                # Update callbacks AND partial_transcriptions setting for this request
                                self.whisper_wrapper.update_callbacks(
                                    on_transcription=TranscriptionCallback(capture_transcription),
                                    on_event=EventCallback(capture_event),
                                    partial_transcriptions=request.stream
                                )
                                
                                # Update VAD setting if provided
                                vad_value = getattr(request, 'vad_value', None)
                                if vad_value is not None:
                                    self.logger.info(f"Updating VAD hangover to custom value: {vad_value}")
                                    self.whisper_wrapper.set_vad_len_hangover(vad_value)
                            
                            wrapper = self.whisper_wrapper
                        
                        # Start processing
                        await asyncio.to_thread(wrapper.start)
                        
                        # Calculate audio duration
                        audio_duration_sec = len(raw_pcm) / (sampling_rate * channels * 2)  # 2 bytes per sample (16-bit)
                        self.logger.info(f"Audio duration: {audio_duration_sec:.1f}s ({len(raw_pcm)} bytes)")
                        
                        # Write all audio at once - let continuous mode handle chunking
                        buf = (ctypes.c_uint8 * len(raw_pcm)).from_buffer_copy(raw_pcm)
                        wrapper.lib.input_stream_write_buffer(wrapper.stream, buf, len(raw_pcm))
                        wrapper.lib.input_stream_set_use_audio_file(wrapper.stream, True)
                        self.logger.info(f"Wrote {len(raw_pcm)} bytes of raw PCM to input stream")
                        
                        # For short audio files (< 10 seconds), add a small delay to allow C++ processing to complete
                        # This prevents the callback from being lost when we stop too quickly
                        if audio_duration_sec < 10.0:
                            wait_time = audio_duration_sec * 0.25
                            self.logger.info(f"Short audio file detected ({audio_duration_sec:.1f}s), waiting {wait_time:.2f}s for processing...")
                            await asyncio.sleep(wait_time)
                        
                        # Stop processing and reset for next use (but don't close the singleton wrapper)
                        self.logger.info('stopping and resetting wrapper (keeping singleton alive)')
                        await asyncio.to_thread(wrapper.stop_and_reset)

                        # Use the results
                        if transcription_results:
                            if request.stream:
                                # Streaming mode: chunks were already sent immediately in callback
                                # No need to send another final message - it was already sent when is_final=True
                                self.logger.info(f'Streaming mode completed: {len(transcription_results)} chunks sent via callback')
                            else:
                                # Non-streaming mode: concatenate all results and send as single final result
                                # The library returns results quickly, so concatenate and send immediately
                                full_text = ' '.join(transcription_results)
                                self.logger.info(f'Concatenated {len(transcription_results)} chunks into final result')
                                
                                result = TranscriptionsResult.create_result(
                                    text=full_text,
                                    language=request.language or "en",
                                    result_type="transcript.text.done",
                                    stream=False,
                                    sync_id=request.sync_id
                                )
                                
                                await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
                                self.logger.info(f'Sent final result immediately: {len(full_text)} characters')
                        else:
                            # No results, send an empty transcription
                            self.logger.warning("No transcription results received from engine")
                            await self.send_error(
                                Config.ASR_TRANSCRIPTION_OUT,
                                'ASR engine returned no transcription result.',
                                sync_id=request.sync_id
                            )

                    except Exception as e:
                        self.logger.error(f"Error using ASR engine: {e}", exc_info=True)
                        await self.send_error(
                            Config.ASR_TRANSCRIPTION_OUT,
                            f'ASR engine error: {e}',
                            sync_id=request.sync_id
                        )
                else:
                    self.logger.warning("ASR engine not available")
                    await self.send_error(
                        Config.ASR_TRANSCRIPTION_OUT,
                        'ASR engine is not available. The engine failed to initialize at startup.',
                        sync_id=request.sync_id
                    )
                
        except Exception as e:
            self.logger.error(f'Error processing file transcription: {e}', exc_info=True)
            raise
    
    async def _send_streaming_chunk(self, text: str, language: str, sync_id: str):
        """Send a streaming chunk immediately (called from callback)."""
        result = TranscriptionsResult.create_result(
            text=text,
            language=language,
            result_type="transcript.text.delta",
            stream=True,
            sync_id=sync_id
        )
        
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        self.logger.debug(f'Sent immediate streaming chunk: "{text[:50]}..."')
    
    async def _send_streaming_chunk_with_type(self, text: str, language: str, sync_id: str, result_type: str):
        """Send a streaming chunk with specific result type (called from callback)."""
        result = TranscriptionsResult.create_result(
            text=text,
            language=language,
            result_type=result_type,
            stream=True,
            sync_id=sync_id
        )
        
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        self.logger.info(f'Sent streaming chunk ({result_type}): "{text[:50]}..."')
    
    async def send_streaming_results(self, request: TranscriptionsCreateRequest):
        """Send streaming transcription results (deltas + final)."""
        # Simulate streaming results with mock data (DEV mode)
        # Send deltas (incremental text additions) followed by final complete text
        deltas = [
            "Hello",
            " world",
            ",",
            " this",
            " is",
            " a",
            " streaming",
            " transcription",
            " test",
            "."
        ]
        
        full_text = ""
        for i, delta in enumerate(deltas):
            full_text += delta
            is_final = (i == len(deltas) - 1)
            result_type = "transcript.text.done" if is_final else "transcript.text.delta"
            
            # For deltas, send only the incremental text
            # For final, send the complete text
            text_to_send = full_text if is_final else delta
            
            result = TranscriptionsResult.create_result(
                text=text_to_send,
                language=request.language or "en",
                result_type=result_type,
                stream=True,
                sync_id=request.sync_id
            )
            
            await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
            self.logger.debug(f'Sent streaming {result_type}: "{text_to_send}"')
            
            if not is_final and self.dev_mode:
                await asyncio.sleep(0.2)  # Simulate processing time
    
    async def send_streaming_result(self, session_id: str, text: str, is_final: bool = False):
        """Send a streaming result for a specific session."""
        # Get session language, but don't fail if session doesn't exist
        # (callback might be called after session cleanup)
        language = "en"
        if session_id in self.active_sessions:
            session = self.active_sessions[session_id]
            language = session.get('language', "en")
        else:
            self.logger.warning(f'Session {session_id} not found when sending result, using default language')
        
        result_type = "transcript.text.done" if is_final else "transcript.text.delta"
        
        result = TranscriptionsResult.create_result(
            text=text,
            language=language,
            result_type=result_type,
            stream=True,
            session_id=session_id
        )
        
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        self.logger.info(f'Sent streaming result ({result_type}) for session {session_id}: {text[:50]}...')
    
    async def send_speech_event(self, session_id: str, event: str):
        """Send a speech detection event for a specific session."""
        # Get session language, but don't fail if session doesn't exist
        language = "en"
        if session_id in self.active_sessions:
            session = self.active_sessions[session_id]
            language = session.get('language', "en")
        else:
            self.logger.warning(f'Session {session_id} not found when sending event, using default language')
        
        result = TranscriptionsResult.create_result(
            text=event,
            language=language,
            result_type="transcript.event",
            stream=True,
            session_id=session_id
        )
        
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        self.logger.info(f'Sent speech event ({event}) for session {session_id}')
    
    async def send_final_result(self, request: TranscriptionsCreateRequest, text: str = None):
        """Send final transcription result (non-streaming)."""
        # Use provided text or default mock text
        if text is None:
            text = "The transcribed text. The sky is blue"
        
        result = TranscriptionsResult.create_result(
            text=text,
            language=request.language or "en",
            result_type="transcript.text.done",
            stream=False,
            sync_id=request.sync_id
        )
        
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        self.logger.info(f'Sent final result: {text}')
    

    async def create_streaming_session(self, request: TranscriptionsCreateRequest):
        """Create a live streaming transcription session."""
        # Clean up any existing sessions first (only one session allowed at a time)
        await self._cleanup_all_sessions()
        
        import uuid
        session_id = str(uuid.uuid4())
        
        # Get sampling rate and channels from request
        sampling_rate = getattr(request, 'sampling_rate', 16000)
        channels = getattr(request, 'channels', 1)
        
        session_data = {
            'model': request.model,
            'language': request.language,
            'created_at': time.time(),
            'audio_chunks': [],
            'sampling_rate': sampling_rate,
            'channels': channels,
            'recorder': None                    # For on device recording
        }
        
        if not self.dev_mode and self.asr_engine:
            try:
                # Initialize the ASR engine for streaming
                self.logger.info("Initializing ASR engine for streaming session")
                
                # Get model configuration
                model_info = next((m for m in self.model_config if m["name"] == request.model), None)
                if not model_info:
                    self.logger.error(f"Model configuration not found for {request.model}")
                    raise ValueError(f"Model configuration not found for {request.model}")
                
                # Extract model paths from config
                encoder_path = model_info.get("model_path", "").encode("utf-8")
                decoder_path = model_info.get("decoder_path", "").encode("utf-8")
                vocab_path = model_info.get("vocab_path", "").encode("utf-8")
                speech_path = model_info.get("speech_path", "").encode("utf-8")
                model_path = b""  # Empty for now
                
                # Get the event loop to schedule coroutines from C callback thread
                loop = asyncio.get_event_loop()
                
                # Set up the callback for this session
                def capture_transcription(results, count, user_data):
                    # Log IMMEDIATELY when callback is invoked
                    import sys
                    print(f"[CALLBACK INVOKED] session={session_id}, count={count}", file=sys.stderr, flush=True)
                    try:
                        # Parse the key-value pairs from the C callback
                        result_dict = {}
                        for i in range(count):
                            kv = results[i]
                            key = kv.key.decode("utf-8") if kv.key else ""
                            value = kv.value.decode("utf-8") if kv.value else ""
                            result_dict[key] = value
                            print(f"[CALLBACK KV] {key}={value[:50] if len(value) > 50 else value}", file=sys.stderr, flush=True)
                        
                        # Extract the transcription text and is_final flag
                        transcription_text = result_dict.get("transcription", "")
                        is_final_str = result_dict.get("is_final", "true")  # Note: key is "is_final" not "isFinal"
                        # Handle various possible values: "true", "True", "1", "false", "False", "0"
                        is_final = is_final_str.lower() in ["true", "1"]

                        if transcription_text:
                            self.logger.info(f"[CALLBACK] Received transcription for session {session_id}: is_final={is_final} (raw='{is_final_str}'), text={transcription_text[:50]}...")
                            
                            # Send the result immediately to the client
                            # Use call_soon_threadsafe since callback is from C thread
                            loop.call_soon_threadsafe(
                                asyncio.create_task,
                                self.send_streaming_result(session_id, transcription_text, is_final=is_final)
                            )
                        else:
                            self.logger.debug(f"[CALLBACK] Empty transcription text for session {session_id}, is_final={is_final}")
                    except Exception as e:
                        self.logger.error(f"[CALLBACK] Error in capture_transcription for session {session_id}: {e}", exc_info=True)

                # Define event callback type
                EventCallback = CFUNCTYPE(None, c_int32, c_void_p)

                def capture_event(event_code, user_data):
                    """Callback to receive ASR events."""
                    try:
                        if event_code == wrapper.EVENT_SPEECH_STARTED:
                            self.logger.info(f"[ASR Event] Speech started for session {session_id}")
                            loop.call_soon_threadsafe(
                                asyncio.create_task,
                                self.send_speech_event(session_id, "speech_start")
                            )
                        elif event_code == wrapper.EVENT_SPEECH_ENDED:
                            self.logger.info(f"[ASR Event] Speech ended for session {session_id}")
                            loop.call_soon_threadsafe(
                                asyncio.create_task,
                                self.send_speech_event(session_id, "speech_end")
                            )
                            # Stop whisper + recorder on a background thread so we don't block
                            stop_thread = threading.Thread(
                                target=self._stop_and_restart_recorder,
                                args=(wrapper, loop),
                                daemon=True
                            )
                            stop_thread.start()
                            self.logger.info(f"[ASR Event] Stop+restart dispatched to background thread for session {session_id}")
                        else:
                            self.logger.info(f"[ASR Event] Unknown event code: {event_code} for session {session_id}")
                    except Exception as e:
                        self.logger.error(f"Error in event callback: {e}", exc_info=True)

                # Get or create singleton WhisperWrapper instance
                async with self.whisper_wrapper_lock:
                    if self.whisper_wrapper is None:
                        self.logger.info("Creating singleton WhisperWrapper instance...")
                        language = request.language.encode("utf-8") if request.language else None
                        
                        # Determine continuous mode based on VAD parameter
                        vad_value = getattr(request, 'vad_value', None)
                        if vad_value is not None:
                            # If VAD is provided, use non-continuous mode with custom VAD
                            continuous_mode = False
                            vad_hangover = vad_value
                            self.logger.info(f"Using VAD mode for streaming: continuous=False, vad_hangover={vad_hangover}")
                        else:
                            # Default behavior: continuous mode with environment VAD
                            continuous_mode = True
                            vad_hangover = self.vad_len_hangover
                            self.logger.info(f"Using continuous mode for streaming: continuous=True, vad_hangover={vad_hangover}")
                        
                        logger.info(f"asr-service streaming session language={language}, continuous={continuous_mode}, partial_transcriptions=True")
                        
                        # For streaming sessions, always enable partial transcriptions
                        self.whisper_wrapper = self.asr_engine.WhisperWrapper(
                            language=language,
                            translation_enabled=False,
                            continuous=continuous_mode,
                            partial_transcriptions=True,  # Always True for streaming sessions
                            on_transcription=TranscriptionCallback(capture_transcription),
                            on_event=EventCallback(capture_event),
                            on_error=None
                        )
                        
                        # Initialize Whisper with model paths
                        await asyncio.to_thread(self.whisper_wrapper._init_whisper,
                            encoder_path=encoder_path,
                            decoder_path=decoder_path,
                            vocab_path=vocab_path,
                            speech_path=speech_path,
                            model_path=model_path
                        )
                        
                        # Set VAD length hangover (custom or environment value)
                        self.whisper_wrapper.set_vad_len_hangover(vad_hangover)
                        
                        self.logger.info("Singleton WhisperWrapper created and initialized")
                    else:
                        self.logger.info("Reusing existing singleton WhisperWrapper instance")
                        # Update callbacks AND partial_transcriptions setting for this session
                        self.whisper_wrapper.update_callbacks(
                            on_transcription=TranscriptionCallback(capture_transcription),
                            on_event=EventCallback(capture_event),
                            partial_transcriptions=True  # Always True for streaming sessions
                        )
                        
                        # Update VAD setting if provided
                        vad_value = getattr(request, 'vad_value', None)
                        if vad_value is not None:
                            # Ensure we convert to int, even if it comes as float
                            vad_value_int = int(float(vad_value))
                            self.logger.info(f"Updating VAD hangover for streaming session to custom value: {vad_value_int}")
                            self.whisper_wrapper.set_vad_len_hangover(vad_value_int)
                    
                    wrapper = self.whisper_wrapper
                
                # Start processing
                await asyncio.to_thread(wrapper.start)
                
                # Store reference to the singleton wrapper in the session
                session_data['whisper_wrapper'] = wrapper
                
                self.logger.info(f"ASR engine initialized for streaming session {session_id}")
            except Exception as e:
                self.logger.error(f"Error initializing ASR engine for streaming: {e}", exc_info=True)
        else:
            if self.dev_mode:
                self.logger.info("Running in dev mode - creating session without ASR engine")
            else:
                self.logger.warning("ASR engine not available")
                await self.send_error(
                    Config.ASR_TRANSCRIPTION_OUT,
                    'ASR engine is not available. The engine failed to initialize at startup.',
                    sync_id=request.sync_id
                )
                return
        
        # Store session info
        self.active_sessions[session_id] = session_data
        
        self.logger.info(f'Created streaming session: {session_id}')

        self.logger.info(request)

        params = []
        mic_name = None
        if hasattr(request, "parameters") and request.parameters:
            if isinstance(request.parameters, str):
                try:
                    params = json.loads(request.parameters)
                except json.JSONDecodeError:
                    self.logger.warning(f"Could not parse parameters: {request.parameters}")
            elif isinstance(request.parameters, list):
                params = request.parameters

        on_device_mic = False
        for param in params:
            if isinstance(param, dict):
                if param.get("key") == "on_device_recording":
                    value = param.get("value", "false")
                    on_device_mic = value.lower() in ["true", "1", "yes"]
                    self.logger.info(f"on_device_recording set to: {on_device_mic}")
                elif param.get("key") == "on_device_recorder_name":
                    mic_name = param.get("value") or None
                    self.logger.info(f"on_device_recorder_name set to: {mic_name}")

        # Create recorder if we want on_device_microphone
        if on_device_mic:
            # List available devices for debugging
            self.logger.info("=== Listing available audio input devices for on_device_recording ===")
            try:
                import re
                def _base_name(n: str) -> str:
                    """Strip trailing (hw:X,Y) suffix and normalise to lowercase for fuzzy matching."""
                    return re.sub(r'\s*\(hw:\d+,\d+\)\s*$', '', n).strip().lower()

                available_devices = Recorder.list_input_devices()
                num_devices = len(available_devices)
                self.logger.info(f"Found {num_devices} input devices:")
                for dev in available_devices:
                    self.logger.info(f"  - {dev['index']}: {dev['name']} ({dev['default_samplerate']} Hz, {dev['max_input_channels']} channels)")

                if num_devices > 0:
                    if mic_name is not None:
                        # Fuzzy match: strip (hw:X,Y) so device index changes don't break selection
                        requested_base = _base_name(mic_name)
                        matched = next(
                            (d['name'] for d in available_devices if requested_base in _base_name(d['name'])),
                            None
                        )
                        if matched:
                            self.logger.info(f'Found requested recorder device: {matched}')
                            mic_name = matched
                        else:
                            self.logger.warning(
                                f"Requested recorder '{mic_name}' not found in available devices, "
                                f"falling back to first available"
                            )
                            mic_name = available_devices[0]['name']
                            self.logger.info(f"using first available {mic_name}")
                    else:
                        # No device specified — use first available
                        mic_name = available_devices[0]['name']
                        self.logger.info(f"using first available {mic_name}")
            except Exception as e:
                self.logger.error(f"Error listing devices: {e}")
            
            if mic_name is None:
                # Try to find an available device dynamically
                # Try common device names in order of preference
                #Recorder.list_input_devices()
                
                device_search_names = ["pulse", "Yeti", "usb", "default", "microphone", "mic", "audio", "usb", "built-in", "internal"]

                for search_name in device_search_names:
                    found_device = Recorder.get_mic_by_name(search_name)
                    if found_device:
                        mic_name = search_name
                        self.logger.info(f"Found audio device using search term '{search_name}': {found_device}")
                        break
            
            if mic_name is None:
                self.logger.warning("Could not find any audio input device. On-device recording will not be available.")
                self.logger.warning("Please ensure an audio input device is connected and accessible.")
            else:
                try:
                    self._start_recorder(mic_name, session_id)                        
                except Exception as e:
                    self.logger.error(f"Error initializing recorder: {e}", exc_info=True)

        # Return session_id via HTTP response only (no WebSocket message)
        # The WebSocket should ONLY be used for actual transcription results
        result = TranscriptionsResult.create_result(
            text="Successfully started Transcription Engine. Please connect to the WebSocket to send audio data & receive transcription output.",
            language=request.language or "en",
            result_type="session.created",  # Not a transcription message
            stream=True,
            session_id=session_id,
            sync_id=request.sync_id
        )

        ### DEBUG using speaker playback
        # Let pulse audio automatically decide on speaker
        # speaker_name = "pulse"
        # from utils.speaker import Speaker
        # # Create speaker for output device
        # speaker = None
        # try:
        #     # Stop all existing speakers, some might be left hanging after unplugging
        #     for name, (prev_speaker, prev_speaker_thread) in self.speakers.items():
        #         if prev_speaker:
        #             # Close previous stream using the same speaker before playing new stream
        #             prev_speaker.clear_buffer()
        #             prev_speaker.mark_buffer_finished()
        #             prev_speaker_thread.join()

        #     Speaker.refresh_devices()
        #     Speaker.list_output_devices()
        #     speaker = Speaker(speaker_name)
        #     device_name = speaker.get_device_name()                       

        #     speaker_thread = threading.Thread(target=speaker.play_speaker_buffer, daemon=True)
        #     speaker_thread.start()
        #     self.speakers[device_name] = (speaker, speaker_thread)

        # except Exception as e:
        #     logger.error(f"Speaker Initialization Failed: {e}")
        speaker = None

        asyncio.create_task(self.handle_recording_audio(session_id, speaker))
        await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())

    def _start_recorder(self, mic_name: str, session_id: str) -> None:
        """Create (or reuse) the recorder and start recording in a background thread.

          - Early return if already recording on the same device (no PortAudio reinit).
          - Stop old recorder and create a brand-new instance when switching devices
            or restarting after a speech-end event.  Never calls reset().
        """
        import re
        def _base_name(n: str) -> str:
            return re.sub(r'\s*\(hw:\d+,\d+\)\s*$', '', n).strip().lower()

        # --- Early return: recorder already running on the same device ---
        if (
            self.recorder is not None
            and not self.recorder.done_recording()
            and self.recorder_thread is not None
            and self.recorder_thread.is_alive()
            and self.recorder.get_device_name() is not None
            and _base_name(mic_name) in _base_name(self.recorder.get_device_name())
        ):
            idle_sec = time.time() - self.recorder_last_used
            if idle_sec > self.RECORDER_IDLE_TIMEOUT_SEC:
                self.logger.info(
                    f"Recorder idle for {idle_sec:.0f}s (> {self.RECORDER_IDLE_TIMEOUT_SEC}s) "
                    f"— forcing recreation to recover from possible OS device release"
                )
                # Fall through to stop + recreate below
            else:
                self.logger.info(
                    f"Recorder already running on '{self.recorder.get_device_name()}' — reusing"
                )
                return

        # --- Stop existing recorder if one is present ---
        if self.recorder is not None:
            current_device = self.recorder.get_device_name()
            self.logger.info(
                f"Stopping recorder on '{current_device}' before starting new one for '{mic_name}'"
            )
            if not self.recorder.done_recording():
                self.recorder.stop_recording()
            if self.recorder_thread and self.recorder_thread.is_alive():
                self.recorder_thread.join(timeout=5.0)
                if self.recorder_thread.is_alive():
                    self.logger.warning("Recorder thread did not stop within 5 s timeout")
            # Give ALSA a moment to fully release the device before reopening it.
            time.sleep(0.3)
            # Destroy old instance — never call reset(), always create fresh
            self.recorder = None
            self.recorder_thread = None

        # --- Create a brand-new Recorder for the requested device ---
        # Query the device's native sample rate so PortAudio can open the stream
        # without a paInvalidSampleRate error.  Many hardware devices (e.g. Yeti Nano
        # at 44100 Hz, LEMANS-EVK at 48000 Hz) do not support 16000 Hz directly.
        # We record at the native rate and resample to 16000 Hz in
        # handle_recording_audio before handing audio to the ASR engine.
        device_native_rate = Recorder.get_device_sample_rate(mic_name)
        record_rate = int(device_native_rate) if device_native_rate else 16000
        self.recorder_sample_rate = record_rate
        self.logger.info(
            f"Creating new Recorder for device '{mic_name}' at {record_rate} Hz (native)"
            + (f" — will resample to 16000 Hz for ASR" if record_rate != 16000 else "")
        )
        # refresh=False: devices were already listed/refreshed above, and the TTS
        # speaker stream may be running — skip sd._terminate() to avoid killing it.
        self.recorder = Recorder(mic_name, record_rate, 1, "int16", 8192, refresh=False)
        if self.recorder.get_device_name() is None:
            self.logger.warning("Recorder initialization failed")
            self.recorder = None
            return

        self.recorder_ready.clear()
        self.recorder_thread = threading.Thread(target=self._run_recorder, daemon=True)
        self.recorder_thread.start()
        self.logger.info(f"Recorder started for session {session_id}: {self.recorder.get_device_name()}")

    def _run_recorder(self) -> None:
        """Run recorder.start_recording() and set recorder_ready when the stream is live."""
        try:
            self.recorder_ready.set()
            self.recorder.start_recording()
        except Exception as e:
            self.logger.error(f"Recorder thread error: {e}", exc_info=True)
        finally:
            self.recorder_ready.clear()

    def _stop_and_restart_recorder(self, wrapper, loop: asyncio.AbstractEventLoop) -> None:
        """Stop whisper + recorder (blocking), then reset and restart the recorder.
        Runs entirely on a background thread so the event callback returns immediately."""
        # 1. Stop whisper
        try:
            wrapper.stop_and_reset()
            self.logger.info("[BG] Whisper stopped and reset")
        except Exception as e:
            self.logger.error(f"[BG] Error stopping whisper: {e}")

        # 2. Stop recorder — capture device params before destroying the instance
        if self.recorder:
            device_name = self.recorder.get_device_name()
            sample_rate = self.recorder_sample_rate
            try:
                self.recorder.stop_recording()
                if self.recorder_thread:
                    self.recorder_thread.join()
                self.logger.info(f"[BG] Recorder stopped: {device_name}")
            except Exception as e:
                self.logger.error(f"[BG] Error stopping recorder: {e}")

            # 3. Destroy old recorder and create a brand-new instance (mirrors the
            # TTS _start_persistent_speaker pattern — never call reset(), always
            # create fresh so the next session gets a clean PortAudio stream).
            try:
                time.sleep(0.3)
                self.recorder = None          # destroy old instance
                self.logger.info(f"[BG] Old recorder destroyed, creating new one for: {device_name}")
                # refresh=False: skip sd._terminate() so the TTS speaker stream is not killed
                self.recorder = Recorder(device_name, sample_rate, 1, "int16", 8192, refresh=False)
                self.recorder_thread = None
                #self.recorder_thread = threading.Thread(target=self._run_recorder, daemon=True)
                #self.recorder_thread.start()
                #self.logger.info(f"[BG] New recorder created and started: {self.recorder.get_device_name()}")
                self.logger.info(f"[BG] New recorder created: {self.recorder.get_device_name()}")
            except Exception as e:
                self.logger.error(f"[BG] Error recreating recorder: {e}")
                self.recorder = None

    async def handle_recording_audio(self, session_id: str, speaker):
        """
        Continuously processes audio chunks from the recorder for a session.
        Runs in the background until the session is closed.
        """
        self.logger.info(f"Starting audio recording handler for session {session_id}")

        session = self.active_sessions.get(session_id)
        if not session:
            self.logger.error(f"Session {session_id} not found in active_sessions")
            return

        if not self.recorder:
            self.logger.error(f"There is no recorder for session {session_id}")
            return

        while session_id in self.active_sessions and not self.recorder.done_recording():
            audio_bytes = self.recorder.get_audio_data()

            # Recorder has not added data yet or recorder is not working
            if not audio_bytes:
                await asyncio.sleep(0.1)
                continue

            # Update last-used timestamp so the idle-timeout logic in _start_recorder
            # knows the recorder is still actively being used.
            self.recorder_last_used = time.time()

            # Resample from the device's native rate to 16000 Hz when they differ.
            # The ASR engine always expects 16 kHz mono int16 PCM.
            if self.recorder_sample_rate != 16000:
                try:
                    audio_bytes = resample_audio(
                        audio_bytes,
                        original_sample_rate=self.recorder_sample_rate,
                        target_sample_rate=16000,
                        original_channels=1,
                        target_channels=1,
                        sample_width=2,   # int16 → 2 bytes per sample
                        is_wav_file=False
                    )
                except Exception as resample_err:
                    self.logger.error(
                        f"Error resampling recorder audio from {self.recorder_sample_rate} Hz "
                        f"to 16000 Hz: {resample_err}",
                        exc_info=True
                    )
                    continue

            session['audio_chunks'].append(audio_bytes)
            self.logger.info(f"Processing {len(audio_bytes)} bytes of audio from recorder for session {session_id}")
            await self.process_audio_chunk(session_id, audio_bytes)

        # Recording has stopped - clear any remaining audio in queue without processing
        self.logger.info(f"Recording stopped for session {session_id}, clearing remaining audio queue")
        self.recorder.clear_queue()
        self.logger.info(f"Audio recording handler stopped for session {session_id}")

    async def handle_session_audio(self, message: str):
        """Handle incoming audio chunks for live streaming."""
        try:
            # Check if the message is a JSON string or binary data
            if message.startswith('{'): 
                # JSON message format
                audio_msg = TranscriptionsSessionAudio.from_json(message)
                session_id = audio_msg.session_id
                
                self.logger.debug(f'Received session audio message: session_id="{session_id}", data_length={len(audio_msg.data) if audio_msg.data else 0}')
                
                if not session_id:
                    self.logger.warning(f'Session ID is empty or missing')
                    return
                
                if session_id not in self.active_sessions:
                    self.logger.warning(f'Session not found: {session_id}. Active sessions: {list(self.active_sessions.keys())}')
                    return
                
                # Decode audio chunk
                audio_bytes = base64_to_bytes(audio_msg.data)
                if not audio_bytes:
                    self.logger.error(f'Failed to decode audio chunk. Data field length: {len(audio_msg.data) if audio_msg.data else 0}')
                    return
            else:
                # Try to parse as JSON first to get session_id
                try:
                    import json
                    data = json.loads(message)
                    session_id = data.get('session_id')
                    
                    # Ignore legacy control messages (not part of API spec)
                    if data.get('type') in ['session.start', 'audio.start', 'audio.end']:
                        self.logger.debug(f"Ignoring legacy control message: {data.get('type')}")
                        return
                        
                except json.JSONDecodeError:
                    # Not JSON, assume it's binary audio data from WebSocket
                    # In this case, we need to get the session_id from the WebSocket connection
                    # For now, use the first active session (this is a simplification)
                    if not self.active_sessions:
                        self.logger.warning(f'No active sessions found for binary audio data')
                        return
                        
                    session_id = list(self.active_sessions.keys())[0]
                    self.logger.debug(f'Using first active session {session_id} for binary audio data')
                    
                    # The message itself is the binary audio data
                    audio_bytes = message.encode('latin1') if isinstance(message, str) else message

            # Store chunk
            session = self.active_sessions[session_id]
            session['audio_chunks'].append(audio_bytes)
            
            self.logger.debug(
                f'Received audio chunk for session {session_id}: '
                f'{len(audio_bytes)} bytes'
            )
            
            # Process audio chunk
            await self.process_audio_chunk(session_id, audio_bytes)
            
        except Exception as e:
            self.logger.error(f'Error handling session audio: {e}', exc_info=True)
    
    async def process_audio_chunk(self, session_id: str, audio_bytes: bytes):
        """Process an audio chunk and send results.
        
        Note: audio_bytes should already be raw PCM data (no headers).
        The API sends raw PCM chunks that are already base64 decoded.
        """
        if self.dev_mode:
            # In development mode, use mock responses
            self.logger.debug("Using mock processing for audio chunk in development mode")
            await asyncio.sleep(0.1)
            
            # Simulate result
            text = f"Partial transcription from chunk"
            
            result = TranscriptionsResult.create_result(
                text=text,
                language="en",
                result_type="transcript.text.delta",
                stream=True,
                session_id=session_id
            )
            
            await self.publish(Config.ASR_TRANSCRIPTION_OUT, result.to_json())
        else:
            # In production mode, use the actual ASR engine
            self.logger.debug(f"Processing audio chunk: {len(audio_bytes)} bytes of raw PCM")
            
            if self.asr_engine and session_id in self.active_sessions:
                try:
                    # Get the session info
                    session = self.active_sessions[session_id]
                    
                    # If the session has a whisper wrapper, use it
                    if 'whisper_wrapper' in session:
                        wrapper = session['whisper_wrapper']
                        
                        # Audio chunks are already raw PCM, no header stripping needed
                        # Just write directly to the stream using write_buffer
                        buf = (ctypes.c_uint8 * len(audio_bytes)).from_buffer_copy(audio_bytes)
                        wrapper.lib.input_stream_write_buffer(wrapper.stream, buf, len(audio_bytes))
                        wrapper.lib.input_stream_set_use_audio_file(wrapper.stream, False)
                        self.logger.debug(f"Wrote {len(audio_bytes)} bytes of raw PCM to input stream for session {session_id}")
                        
                        # The engine should call our callback when it has results
                        # We don't need to do anything else here
                    else:
                        self.logger.warning(f"Session {session_id} doesn't have a whisper wrapper")
                        await self.send_error(
                            Config.ASR_TRANSCRIPTION_OUT,
                            'ASR engine is not initialized for this session.',
                            session_id=session_id
                        )

                except Exception as e:
                    self.logger.error(f"Error processing audio chunk with ASR engine: {e}", exc_info=True)
                    await self.send_error(
                        Config.ASR_TRANSCRIPTION_OUT,
                        f'ASR engine error processing audio: {e}',
                        session_id=session_id
                    )
            else:
                # Engine not available or session not found
                self.logger.warning(f"ASR engine not available or session {session_id} not found")
                await self.send_error(
                    Config.ASR_TRANSCRIPTION_OUT,
                    'ASR engine is not available. The engine failed to initialize at startup.',
                    session_id=session_id
                )
    
    async def handle_close_transcription(self, message: str, session_id: str = None):
        """Handle transcription close request."""
        try:
            if session_id:
                pass
            elif message:
                close_msg = TranscriptionsClose.from_json(message)
                session_id = close_msg.session_id
            else:
                self.logger.info("handle_close_transcription called with no message or session id!")
                return                        

            self.logger.info(f"handle_close_transcription {session_id}")

            if session_id and session_id in self.active_sessions:
                session = self.active_sessions[session_id]
                if self.recorder and not self.recorder.done_recording():
                    self.logger.info(f"Stopping recorder on close for session {session_id}")
                    self.recorder.stop_recording()
                    if self.recorder_thread:
                        await asyncio.to_thread(self.recorder_thread.join)
                    logger.info(f"Recorder stopped on close: {self.recorder.get_device_name()}")
                else:
                    self.logger.info(f"Recorder already stopped or not initialized for session {session_id}")

                if not self.dev_mode and self.asr_engine:
                    try:
                        # Stop and reset the wrapper for this session (but keep singleton alive)
                        if 'whisper_wrapper' in session:
                            self.logger.info(f"Stopping and resetting ASR processing for session {session_id}")
                            #await asyncio.to_thread(session['whisper_wrapper'].stop_and_reset)
                            await asyncio.to_thread(session['whisper_wrapper'].close)  # ← Deinitializes
                            self.whisper_wrapper = None  # ← Clear singleton reference
                    except Exception as e:
                        self.logger.error(f"Error stopping ASR processing: {e}", exc_info=True)

                    # recorder is a singleton on self, already stopped above

                # Cleanup session
                del self.active_sessions[session_id]
                self.logger.info(f'Closed session: {session_id}')
            else:
                self.logger.info('Received close message (no specific session)')

                # Still deinitialize the wrapper if it exists
                if self.whisper_wrapper is not None:
                    self.logger.info("Deinitializing WhisperWrapper (no specific session)...")
                    try:
                        await asyncio.to_thread(self.whisper_wrapper.close)  # ← Deinitializes
                        self.whisper_wrapper = None  # ← Clear singleton reference
                        self.logger.info("WhisperWrapper deinitialized successfully")
                    except Exception as e:
                        self.logger.error(f"Error deinitializing WhisperWrapper: {e}", exc_info=True)
            

        except Exception as e:
            self.logger.error(f'Error handling close: {e}', exc_info=True)
    
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
            
            request = TranscriptionsModelsRequest.from_json(message)
            
            # Build detailed model information for API
            detailed_models = []
            for model in self.model_config:
                detailed_model = {
                    "name": model.get("name", ""),
                    "display_name": model.get("display_name", model.get("name", "")),
                    "description": model.get("description", ""),
                    "version": model.get("version", "1.0.0"),
                    "capabilities": model.get("capabilities", {}),
                    "parameters": model.get("parameters", {})
                }
                detailed_models.append(detailed_model)
            
            # Send models list — result is the plain models array
            response = {
                "sync_id": request.sync_id,
                "result": detailed_models,
                "message_source": "audio_analytics_server"
            }
            
            asyncio.create_task(
                self.publish(Config.ASR_MODELS, json.dumps(response))
            )
            
            self.logger.info(f'Sent models list: {len(detailed_models)} models')

        except Exception as e:
            self.logger.error(f'Error handling models request: {e}', exc_info=True)

    def handle_devices_request(self, message: str):
        """Handle recording devices list request."""
        try:
            data = json.loads(message)
            message_source = data.get('message_source', '')

            # Ignore messages from ourselves (responses)
            if message_source == 'audio_analytics_server':
                self.logger.debug('Ignoring message from server (our own response)')
                return

            sync_id = data.get('sync_id', '')

            # Enumerate available audio input (recording) devices
            recording_devices = []
            try:
                self.logger.info("=== Listing available audio input devices (devices request) ===")
                if self.recorder is None or self.recorder.done_recording():
                    Recorder.refresh_devices()
                available_devices = Recorder.list_input_devices()
                self.logger.info(f"Found {len(available_devices)} input devices:")
                for dev in available_devices:
                    self.logger.info(
                        f"  - {dev['index']}: {dev['name']} "
                        f"({dev['default_samplerate']} Hz, {dev['max_input_channels']} channels)"
                    )
                    recording_devices.append({
                        "index": dev["index"],
                        "name": dev["name"],
                        "default_samplerate": dev["default_samplerate"],
                        "max_input_channels": dev["max_input_channels"],
                    })
            except Exception as dev_err:
                self.logger.error(f"Error listing input devices: {dev_err}")

            response = {
                "sync_id": sync_id,
                "result": recording_devices,
                "message_source": "audio_analytics_server"
            }

            asyncio.create_task(
                self.publish(Config.ASR_DEVICES, json.dumps(response))
            )

            self.logger.info(f'Sent recording devices list: {len(recording_devices)} devices')

        except Exception as e:
            self.logger.error(f'Error handling devices request: {e}', exc_info=True)
