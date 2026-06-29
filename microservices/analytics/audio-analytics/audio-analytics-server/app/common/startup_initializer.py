# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Startup Initializer Module

This module handles the initialization of ASR and TTS services, along with
recorder and speaker utilities, when the container starts. This pre-initialization
helps avoid delays when the first requests are made.
"""

import asyncio
import os
import threading
import time
from typing import Optional, Dict, Any
from common.logger import get_logger
from common.config import Config
from common.model_loader import ModelLoader
from utils.recorder import Recorder
from utils.speaker import Speaker

logger = get_logger(__name__)


class StartupInitializer:
    """
    Handles startup initialization of audio services and utilities.
    """
    
    def __init__(self):
        self.dev_mode = Config.DEV_MODE
        self.initialization_complete = False
        self.asr_engine = None
        self.tts_wrapper_class = None
        self.recorder_ready = False
        self.speaker_ready = False
        # Pre-started recorder instance (reused by ASRService)
        self.recorder_instance: Optional[Recorder] = None
        self.recorder_thread: Optional[threading.Thread] = None
        self.recorder_mic_name: Optional[str] = None
        # Pre-started speaker instance (reused by TTSService)
        self.speaker_instance: Optional[Speaker] = None
        self.speaker_thread: Optional[threading.Thread] = None
        self.speaker_device_name: Optional[str] = None
        
    async def initialize_all(self):
        """
        Initialize all audio services and utilities at startup.
        """
        logger.info("="*60)
        logger.info("Starting Audio Analytics Startup Initialization")
        logger.info("="*60)
        
        start_time = time.time()
        
        try:
            # Initialize in parallel where possible
            await asyncio.gather(
                self._initialize_asr_engine(),
                self._initialize_tts_engine(),
                self._initialize_audio_devices(),
                return_exceptions=True
            )
            
            self.initialization_complete = True
            elapsed = time.time() - start_time
            
            logger.info("="*60)
            logger.info(f"Startup Initialization Complete ({elapsed:.2f}s)")
            logger.info("="*60)
            
        except Exception as e:
            logger.error(f"Error during startup initialization: {e}", exc_info=True)
            # Don't fail startup - services can still work in degraded mode
            
    async def _initialize_asr_engine(self):
        """Initialize ASR engine and load models."""
        logger.info("Initializing ASR engine...")
        
        if self.dev_mode:
            logger.info("ASR: Running in development mode - skipping engine initialization")
            return
            
        try:
            # Import ASR wrapper module
            import sys
            import importlib.util
            
            wrapper_path = os.path.join(
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                "asr_wrapper.py"
            )
            
            if os.path.exists(wrapper_path):
                logger.info(f"ASR: Loading wrapper from {wrapper_path}")
                spec = importlib.util.spec_from_file_location("asr_wrapper", wrapper_path)
                if spec and spec.loader:
                    asr_wrapper_module = importlib.util.module_from_spec(spec)
                    sys.modules["asr_wrapper"] = asr_wrapper_module
                    spec.loader.exec_module(asr_wrapper_module)
                    self.asr_engine = asr_wrapper_module
                    logger.info("ASR: Engine loaded successfully")
                    
                    # Pre-load model configuration
                    model_config = ModelLoader.get_asr_models(self.dev_mode)
                    available_models = [model["name"] for model in model_config]
                    logger.info(f"ASR: Available models: {available_models}")
                    
                else:
                    logger.error("ASR: Failed to load wrapper module specification")
            else:
                logger.warning(f"ASR: Wrapper not found at {wrapper_path}")
                
        except Exception as e:
            logger.error(f"ASR: Error initializing engine: {e}", exc_info=True)
            
    async def _initialize_tts_engine(self):
        """Initialize TTS engine and load models."""
        logger.info("Initializing TTS engine...")
        
        if self.dev_mode:
            logger.info("TTS: Running in development mode - skipping engine initialization")
            return
            
        try:
            # Import TTS wrapper module
            import sys
            import importlib.util
            
            wrapper_path = os.path.join(
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                "tts_wrapper.py"
            )
            
            if os.path.exists(wrapper_path):
                logger.info(f"TTS: Loading wrapper from {wrapper_path}")
                spec = importlib.util.spec_from_file_location("tts_wrapper", wrapper_path)
                if spec and spec.loader:
                    tts_module = importlib.util.module_from_spec(spec)
                    sys.modules["tts_wrapper"] = tts_module
                    spec.loader.exec_module(tts_module)
                    self.tts_wrapper_class = tts_module.TTS
                    logger.info("TTS: Engine loaded successfully")
                    
                    # Pre-load model configuration
                    model_config = ModelLoader.get_tts_models(self.dev_mode)
                    available_models = ModelLoader.convert_tts_models_to_api_format(model_config)
                    logger.info(f"TTS: Available models: {[m['name'] for m in available_models]}")
                    
                else:
                    logger.error("TTS: Failed to load wrapper module specification")
            else:
                logger.warning(f"TTS: Wrapper not found at {wrapper_path}")
                
        except Exception as e:
            logger.error(f"TTS: Error initializing engine: {e}", exc_info=True)
            
    async def _initialize_audio_devices(self):
        """Initialize audio input and output devices."""
        logger.info("Initializing audio devices...")
        
        # Initialize recorder devices
        await self._initialize_recorder_devices()
        
        # Initialize speaker devices  
        await self._initialize_speaker_devices()
        
    async def _initialize_recorder_devices(self):
        """Initialize, test, and pre-start the recorder device."""
        try:
            logger.info("Recorder: Discovering input devices...")

            # Run device discovery in thread to avoid blocking
            available_devices = await asyncio.to_thread(Recorder.list_input_devices)

            if not available_devices:
                logger.warning("Recorder: No input devices available")
                return

            logger.info(f"Recorder: Found {len(available_devices)} input devices")

            # Find the first working device
            test_names = ["pulse", "default", "microphone", "mic", "audio", "usb", "built-in", "internal"]
            mic_name = None
            for name in test_names:
                try:
                    found_device = await asyncio.to_thread(Recorder.get_mic_by_name, name)
                    if found_device:
                        mic_name = name
                        logger.info(f"Recorder: ✓ Device '{name}' -> '{found_device}'")
                        break
                    else:
                        logger.debug(f"Recorder: ✗ Device '{name}' not found")
                except Exception as e:
                    logger.debug(f"Recorder: Error testing device '{name}': {e}")

            if mic_name is None:
                logger.warning("Recorder: No working input devices found")
                return

            # Pre-start the recorder so it is ready for the first request
            self.recorder_mic_name = mic_name
            await asyncio.to_thread(self._start_recorder_sync)

        except Exception as e:
            logger.error(f"Recorder: Error initializing devices: {e}", exc_info=True)

    def _start_recorder_sync(self):
        """Create and start a new recorder instance (runs in a thread pool worker)."""
        try:
            recorder = Recorder(self.recorder_mic_name, 16000, 1, "int16", 8192)
            if recorder.get_device_name() is None:
                logger.warning("Recorder: Recorder initialization failed (device not found)")
                return
            recorder_thread = threading.Thread(
                target=recorder.start_recording,
                daemon=True,
                name="recorder-prewarm"
            )
            recorder_thread.start()
            self.recorder_instance = recorder
            self.recorder_thread = recorder_thread
            self.recorder_ready = True
            logger.info(f"Recorder: Pre-started on device '{recorder.get_device_name()}'")
        except Exception as e:
            logger.error(f"Recorder: Error pre-starting recorder: {e}", exc_info=True)

    def restart_recorder(self):
        """Stop the current recorder and immediately start a fresh one.

        Called by ASRService after a session ends so the recorder is ready
        for the next request without any per-request startup delay.
        """
        try:
            # Stop the old instance if still running
            if self.recorder_instance is not None and not self.recorder_instance.done_recording():
                self.recorder_instance.stop_recording()
            self.recorder_instance = None
            self.recorder_thread = None

            if self.recorder_mic_name is None:
                logger.warning("Recorder: Cannot restart - no mic name stored")
                return

            self._start_recorder_sync()
        except Exception as e:
            logger.error(f"Recorder: Error restarting recorder: {e}", exc_info=True)
            
    async def _initialize_speaker_devices(self):
        """Initialize, test, and pre-start the speaker device."""
        try:
            logger.info("Speaker: Discovering output devices...")

            # Run device discovery in thread to avoid blocking
            available_devices = await asyncio.to_thread(Speaker.list_output_devices)

            if not available_devices:
                logger.warning("Speaker: No output devices available")
                return

            output_devices = [dev for dev in available_devices if dev.get("max_output_channels", 0) > 0]
            logger.info(f"Speaker: Found {len(output_devices)} output devices")

            # Find the first working device (pulse preferred)
            test_names = ["pulse", "default", "speaker", "audio", "usb", "built-in", "internal"]
            device_name = None
            for name in test_names:
                try:
                    found_device = await asyncio.to_thread(Speaker.get_speaker_by_name, name)
                    if found_device:
                        device_name = name
                        logger.info(f"Speaker: ✓ Device '{name}' -> '{found_device}'")
                        break
                    else:
                        logger.debug(f"Speaker: ✗ Device '{name}' not found")
                except Exception as e:
                    logger.debug(f"Speaker: Error testing device '{name}': {e}")

            if device_name is None:
                logger.warning("Speaker: No working output devices found")
                return

            # Pre-start the speaker so it is ready for the first TTS request
            self.speaker_device_name = device_name
            await asyncio.to_thread(self._start_speaker_sync)

        except Exception as e:
            logger.error(f"Speaker: Error initializing devices: {e}", exc_info=True)

    def _start_speaker_sync(self):
        """Create and start a new speaker thread (runs in a thread pool worker)."""
        try:
            Speaker.refresh_devices()
            speaker = Speaker(self.speaker_device_name)
            if speaker.get_device_name() is None:
                logger.warning("Speaker: Initialization failed (device not found)")
                return
            speaker_thread = threading.Thread(
                target=speaker.play_speaker_buffer,
                daemon=True,
                name="speaker-prewarm"
            )
            speaker_thread.start()
            self.speaker_instance = speaker
            self.speaker_thread = speaker_thread
            self.speaker_ready = True
            logger.info(f"Speaker: Pre-started on device '{speaker.get_device_name()}'")
        except Exception as e:
            logger.error(f"Speaker: Error pre-starting speaker: {e}", exc_info=True)

    def restart_speaker_thread(self):
        """Reset the speaker and restart its playback thread.

        Called by TTSService after a request completes so the speaker thread is
        ready before the next TTS request arrives.
        """
        try:
            if self.speaker_instance is None:
                logger.warning("Speaker: Cannot restart thread - no speaker instance")
                return
            # Reset state so the speaker accepts new audio
            self.speaker_instance.reset_for_new_request()
            speaker_thread = threading.Thread(
                target=self.speaker_instance.play_speaker_buffer,
                daemon=True,
                name="speaker-prewarm"
            )
            speaker_thread.start()
            self.speaker_thread = speaker_thread
            logger.info(
                f"Speaker: Thread restarted for device '{self.speaker_instance.get_device_name()}'"
            )
        except Exception as e:
            logger.error(f"Speaker: Error restarting speaker thread: {e}", exc_info=True)
            
    def is_initialization_complete(self) -> bool:
        """Check if startup initialization is complete."""
        return self.initialization_complete
        
    def get_initialization_status(self) -> Dict[str, Any]:
        """Get detailed initialization status."""
        return {
            "complete": self.initialization_complete,
            "dev_mode": self.dev_mode,
            "asr_engine_loaded": self.asr_engine is not None,
            "tts_engine_loaded": self.tts_wrapper_class is not None,
            "recorder_ready": self.recorder_ready,
            "speaker_ready": self.speaker_ready
        }


# Global startup initializer instance
_startup_initializer: Optional[StartupInitializer] = None


def get_startup_initializer() -> StartupInitializer:
    """Get the global startup initializer instance."""
    global _startup_initializer
    if _startup_initializer is None:
        _startup_initializer = StartupInitializer()
    return _startup_initializer


async def initialize_at_startup():
    """Initialize all services at startup."""
    initializer = get_startup_initializer()
    await initializer.initialize_all()
