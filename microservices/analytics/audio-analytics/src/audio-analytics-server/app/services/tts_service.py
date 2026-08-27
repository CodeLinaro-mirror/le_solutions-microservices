# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from typing import Dict, Callable
import asyncio
import os
import sys
import importlib.util
import ctypes
import struct
import time
import wave
import io
import tempfile
import base64
import queue
import threading
from common.base_service import BaseService
from common.redis_client import RedisClient
from common.config import Config
from common.logger import get_logger
from common.model_loader import ModelLoader
from common.resampler import resample_audio
from common.service_coordinator import get_service_coordinator
from models.messages import (
    TTSSynthesizeRequest,
    TTSComplete,
    TTSCancelRequest,
    TTSModelsRequest,
    TTSModelsResponse,
    get_message_type
)
from utils.speaker import Speaker

logger = get_logger(__name__)


class TTSService(BaseService):
    """
    Text-to-Speech (TTS) Service.
    Handles speech synthesis requests and streams audio output.
    """
    
    def __init__(self, redis_client: RedisClient):
        super().__init__(redis_client, "TTS")
        self.dev_mode = Config.DEV_MODE
        self.model_config = ModelLoader.get_tts_models(self.dev_mode)
        self.available_models = []  # Will be populated during initialization
        
        # Initialize TTS engine if not in dev mode
        self.tts_wrapper_class = None
        self.tts_instance = None  # Singleton TTS instance
        self.current_model_path = None  # Track current model to avoid reinit
        self.tts_instance_lock = asyncio.Lock()  # Protect singleton access
        self.audio_chunks = []
        
        self.tts_max_char_len = 1024

        # Persistent speaker instance — started once, lives for the app lifetime
        self.speaker: Speaker = None
        self.speaker_thread: threading.Thread = None
        self.speaker_device_name: str = None
        # Timestamp of the last TTS synthesis — used to detect idle timeout.
        # If the speaker has not been used for SPEAKER_IDLE_TIMEOUT_SEC seconds the
        # stream is considered stale and will be recreated on next use.
        self.speaker_last_used: float = 0.0
        self.SPEAKER_IDLE_TIMEOUT_SEC: int = 5 * 60  # 5 minutes

        # Sequential request queue — one sync_id completes before the next starts.
        # This prevents interleaved audio chunks from concurrent requests on the
        # shared pub/sub channel, which would cause static between sentences.
        self._request_queue: asyncio.Queue = asyncio.Queue()
        self._queue_worker_task: asyncio.Task = None

        # Cancellation flag — set by a tts_cancel message, cleared when the
        # next real synthesize request begins.  Checked at every yield point
        # inside synthesize_speech_real so in-flight synthesis aborts quickly.
        self._cancelled: bool = False
        # Event used to interrupt the chunk-queue consumer loop immediately
        # without waiting for the next chunk to arrive from the TTS thread.
        self._cancel_event: threading.Event = threading.Event()

        # Service coordinator for managing resource conflicts with ASR and T2T
        self.coordinator = get_service_coordinator()

        # Language code mapping
        self.TTS_LANGUAGE_CODE_MAPPING = {
            "melo": Config.MELO_LANGUAGE_CODE_MAP,
            "piper": Config.PIPER_LANGUAGE_CODE_MAP
        }

        # Previous TTS Parameters
        self._prev_tts_param = None

        if not self.dev_mode:
            self.logger.info("Running in production mode - initializing TTS engine")
            try:
                # Try multiple possible locations for the TTS wrapper
                possible_paths = [
                    os.path.join(
                        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "tts_wrapper.py"
                    ),
                    "/usr/src/server/tts_wrapper.py",
                    os.path.join(os.path.dirname(__file__), "..", "..", "tts_wrapper.py")
                ]
                
                self.logger.info(f"Searching for TTS wrapper in {len(possible_paths)} locations...")
                wrapper_path = None
                for path in possible_paths:
                    self.logger.info(f"  Checking: {path}")
                    if os.path.exists(path):
                        wrapper_path = path
                        self.logger.info(f"  ✓ Found TTS wrapper at {path}")
                        break
                    else:
                        self.logger.info(f"  ✗ Not found at {path}")
                
                if wrapper_path:
                    self.logger.info(f"Loading TTS wrapper from {wrapper_path}")
                    # Add the wrapper directory to Python path so wrapper_utils can be imported
                    wrapper_dir = os.path.dirname(wrapper_path)
                    if wrapper_dir not in sys.path:
                        sys.path.insert(0, wrapper_dir)
                        self.logger.info(f"Added {wrapper_dir} to Python path")
                    # Import the module dynamically
                    spec = importlib.util.spec_from_file_location("tts_wrapper", wrapper_path)
                    if spec and spec.loader:
                        self.logger.info("Creating module from spec...")
                        tts_module = importlib.util.module_from_spec(spec)
                        sys.modules["tts_wrapper"] = tts_module
                        self.logger.info("Executing module...")
                        spec.loader.exec_module(tts_module)
                        self.logger.info("Module executed successfully")
                        # Get the TTS class from the module
                        if hasattr(tts_module, 'TTS'):
                            self.tts_wrapper_class = tts_module.TTS
                            self.logger.info("✓ TTS wrapper class loaded successfully")
                            # Try to instantiate to check if .so files are accessible
                            try:
                                test_instance = self.tts_wrapper_class()
                                self.logger.info("✓ TTS wrapper instantiated successfully (library .so files are OK)")
                                del test_instance
                            except Exception as inst_error:
                                self.logger.error(f"✗ TTS wrapper instantiation failed (library .so files may be bad): {inst_error}", exc_info=True)
                                self.tts_wrapper_class = None
                        else:
                            self.logger.error("✗ TTS class not found in wrapper module")
                    else:
                        self.logger.error("✗ Failed to load TTS wrapper module specification")
                else:
                    self.logger.error(f"✗ TTS wrapper not found in any of the {len(possible_paths)} locations")
            except Exception as e:
                self.logger.error(f"✗ Error initializing TTS wrapper: {e}", exc_info=True)
        else:
            self.logger.info("Running in development mode - using mock responses")
        
    def get_subscriptions(self) -> Dict[str, Callable]:
        """Subscribe to TTS input channels."""
        return {
            Config.TTS_TEXT_IN: self.handle_message_safely(self.handle_tts_input),
            Config.TTS_MODELS: self.handle_message_safely(self.handle_models_request),
            Config.TTS_DEVICES: self.handle_message_safely(self.handle_devices_request),
        }

    async def handle_close_request(self):
        """Handle a tts_close message: cancel in-flight synthesis, deinitialize the TTS
        instance, and release DSP/NPU resources."""
        self.logger.info('TTS close request received — deinitializing TTS instance')

        # Cancel any in-flight or queued synthesis first
        self.handle_cancel_request()

        # Deinitialize the singleton TTS instance
        async with self.tts_instance_lock:
            if self.tts_instance is not None:
                try:
                    self.logger.info("Deinitializing TTS instance on close...")
                    await asyncio.to_thread(self.tts_instance.deinit)
                    await asyncio.sleep(0.1)
                    self.tts_instance = None
                    self.current_model_path = None
                    self.logger.info("TTS instance deinitialized on close")
                except Exception as e:
                    self.logger.error(f"Error deinitializing TTS instance on close: {e}", exc_info=True)
                    self.tts_instance = None
                    self.current_model_path = None
            else:
                self.logger.info("TTS close: no active instance to deinitialize")

        # Mark keep_alive as False so the next service switch triggers a full cleanup
        self.coordinator.set_tts_keep_alive(False)
        self.logger.info("TTS close complete")

    def handle_cancel_request(self):
        """Handle a tts_cancel message: set the cancellation flag, drain the
        pending request queue, clear the speaker buffer, and signal any
        in-flight chunk-consumer loop to exit immediately."""
        self.logger.info('TTS cancel request received — draining queue and stopping playback')
        self._cancelled = True
        self._cancel_event.set()

        # Drain every pending synthesize request from the queue so the worker
        # does not start processing them after the cancel.
        drained = 0
        while not self._request_queue.empty():
            try:
                self._request_queue.get_nowait()
                self._request_queue.task_done()
                drained += 1
            except Exception:
                break
        if drained:
            self.logger.info(f'Drained {drained} pending TTS request(s) from queue')

        # Clear the speaker buffer so on-device audio stops immediately.
        if self.speaker is not None:
            try:
                self.speaker.clear_buffer()
                self.logger.info('Speaker buffer cleared')
            except Exception as e:
                self.logger.error(f'Error clearing speaker buffer: {e}', exc_info=True)
    
    async def initialize(self):
        """Initialize TTS engine and load models."""
        self.logger.info('Initializing TTS service...')
        
        if not self.dev_mode and self.tts_wrapper_class:
            try:
                self.setup_tts_models()
                self.logger.info("TTS wrapper class is ready for use")
            except Exception as e:
                self.logger.error(f"Error initializing TTS engine: {e}", exc_info=True)
                self.setup_mock_models()
        else:
            self.setup_mock_models()
        
        self.logger.info(f'Available models: {[m["name"] for m in self.available_models]}')

        # Start the sequential queue worker
        self._queue_worker_task = asyncio.create_task(self._request_queue_worker())
        self.logger.info("TTS sequential request queue worker started")

        # Pre-warm the default TTS model so the first real request hits the
        # warm path (cached .qnn file) instead of running generate_model().
        # This runs in the background so it does not block service startup.
        # if not self.dev_mode and self.tts_wrapper_class and self.available_models:
        #     asyncio.create_task(self._prewarm_default_model())

        # NOTE: The persistent speaker is started lazily on the first
        # on_device_playback=True request rather than eagerly here.
        # Starting it at init time calls sd._terminate() (via
        # list_output_devices) which can interfere with the TTS C library's
        # own PortAudio usage and produce garbled audio on stream-back
        # (on_device_playback=False) requests.

    def _start_persistent_speaker(self, device_name: str = None) -> None:
        """Create (or reuse / switch) the Speaker and start its stream thread.

        Device selection priority:
          1. *device_name* argument — typically ``request.output_speaker_name``
          2. First available output device reported by sounddevice
          3. ``"pulse"`` / ``"usb"`` / ``"default"`` fallbacks

        Three cases are handled without ever calling sd._terminate() unnecessarily:

        Case 1 — speaker stream is alive on the correct device → no-op.
        Case 2 — speaker object exists on the same device but the thread died
                  (e.g. because the recorder called sd._terminate()) → restart
                  the thread only; the Speaker object is reused as-is.
        Case 3 — device changed or no speaker yet → resolve device via
                  Speaker.list_output_devices() (which calls sd._terminate once)
                  then create a fresh Speaker with refresh=False.
        """
        import re
        def _base_name(n):
            return re.sub(r'\s*\(hw:\d+,\d+\)\s*$', '', n).strip().lower()

        # ── Case 1: stream alive on the correct device ────────────────────────
        if (self.speaker is not None
                and self.speaker.is_running()
                and self.speaker_device_name is not None):
            if device_name is None or _base_name(device_name) in _base_name(self.speaker_device_name):
                idle_sec = time.time() - self.speaker_last_used
                if idle_sec > self.SPEAKER_IDLE_TIMEOUT_SEC:
                    self.logger.info(
                        f"Speaker idle for {idle_sec:.0f}s (> {self.SPEAKER_IDLE_TIMEOUT_SEC}s) "
                        f"— forcing recreation to recover from possible OS device release"
                    )
                    # Fall through to Case 2 to signal + restart the thread
                else:
                    self.logger.info(
                        f"Persistent speaker already running on '{self.speaker_device_name}' — reusing"
                    )
                    return

        # ── Case 2: same device, stream not running → restart thread ──────────
        # Covers two sub-cases:
        #   a) Thread already dead — stream died and thread exited cleanly.
        #   b) Thread alive but stream dead — sd._terminate() killed the
        #      PortAudio stream while the thread is still blocked on
        #      _stop_event.wait().  We must signal the thread to exit before
        #      restarting so there is never more than one thread per speaker.
        # In both cases the Speaker object is reused; play_speaker_buffer()
        # opens a fresh RawOutputStream without any sd._terminate() call.
        if (self.speaker is not None
                and self.speaker_device_name is not None
                and (device_name is None
                     or _base_name(device_name) in _base_name(self.speaker_device_name))):
            if self.speaker_thread is not None and self.speaker_thread.is_alive():
                self.logger.info(
                    f"Speaker stream dead but thread alive — signaling exit on "
                    f"'{self.speaker_device_name}'"
                )
                self.speaker._stop_event.set()
                self.speaker_thread.join(timeout=2.0)
            self.logger.info(
                f"Restarting speaker thread on '{self.speaker_device_name}' (no device refresh)"
            )
            self.speaker_thread = threading.Thread(
                target=self.speaker.play_speaker_buffer,
                daemon=True,
                name="speaker-stream",
            )
            self.speaker_thread.start()
            self.logger.info(f"Speaker thread restarted: {self.speaker_device_name}")
            return

        # ── Case 3: resolve (possibly new) device and create a fresh Speaker ──
        resolved_name = None
        try:
            # list_output_devices() calls sd._terminate() + sd._initialize()
            output_devices = Speaker.list_output_devices()
            num_devices = len(output_devices)
            self.logger.info(f"Found {num_devices} output devices:")
            for dev in output_devices:
                self.logger.info(
                    f"  - {dev['index']}: {dev['name']} "
                    f"({dev['default_samplerate']} Hz, {dev['max_output_channels']} channels)"
                )
        except Exception as e:
            self.logger.error(f"Error listing output devices: {e}", exc_info=True)
            output_devices = []
            num_devices = 0

        if num_devices > 0:
            if device_name is not None:
                requested_base = _base_name(device_name)
                matched = next(
                    (d['name'] for d in output_devices
                     if requested_base in _base_name(d['name'])),
                    None
                )
                if matched:
                    self.logger.info(f'Found requested device: {matched}')
                    resolved_name = matched
                else:
                    resolved_name = output_devices[0]['name']
                    self.logger.info(
                        f"Requested device '{device_name}' not found, "
                        f"using first available {resolved_name}"
                    )
            else:
                resolved_name = output_devices[0]['name']
                self.logger.info(f"using first available {resolved_name}")

        if resolved_name is None:
            for candidate in ["pulse", "usb", "default"]:
                found = Speaker.get_speaker_by_name(candidate)
                if found:
                    resolved_name = found
                    self.logger.info(
                        f"Selected speaker device '{resolved_name}' (matched '{candidate}')"
                    )
                    break

        if resolved_name is None:
            self.logger.error("No audio output device found — persistent speaker not started")
            self.speaker = None
            return

        # --- stop existing speaker if switching to a different device ---
        if self.speaker is not None:
            self.logger.info(
                f"Switching speaker from '{self.speaker_device_name}' to '{resolved_name}'"
            )
            try:
                self.speaker.stop()
                if self.speaker_thread and self.speaker_thread.is_alive():
                    self.speaker_thread.join(timeout=2.0)
            except Exception as e:
                self.logger.error(f"Error stopping old speaker: {e}", exc_info=True)
            self.speaker = None
            self.speaker_thread = None
            self.speaker_device_name = None

        # --- start new speaker ---
        # refresh=False: Speaker.list_output_devices() already called sd._terminate() above
        try:
            self.speaker = Speaker(resolved_name, refresh=False)
            self.speaker_device_name = self.speaker.get_device_name()
            if self.speaker_device_name is None:
                self.logger.error(f"Speaker device '{resolved_name}' not found after init")
                self.speaker = None
                return
            self.speaker_thread = threading.Thread(
                target=self.speaker.play_speaker_buffer,
                daemon=True,
                name="speaker-stream",
            )
            self.speaker_thread.start()
            self.logger.info(f"Persistent speaker started: {self.speaker_device_name}")
        except Exception as e:
            self.logger.error(f"Failed to start persistent speaker: {e}", exc_info=True)
            self.speaker = None
            self.speaker_thread = None
            self.speaker_device_name = None
    
    def setup_mock_models(self):
        """Set up TTS models for the API format."""
        # Convert the model config to the format expected by the API
        self.available_models = ModelLoader.convert_tts_models_to_api_format(self.model_config)
    
    def setup_tts_models(self):
        """Set up TTS models based on the engine capabilities."""
        # Convert the model config to the format expected by the API
        self.available_models = ModelLoader.convert_tts_models_to_api_format(self.model_config)

    async def _prewarm_default_model(self):
        """Pre-warm the TTS engine for the default (first) model at startup.

        This runs `init_dir` in a background thread so that:
        - The packed .qnn cache file is generated/validated before the first
          real request arrives.
        - Subsequent requests skip `generate_model()` entirely and go straight
          to `init_model_file`, which is dramatically faster.

        The instance is stored in the singleton so the first real request
        reuses it without any re-initialization.
        """
        try:
            # Resolve the model path for the first available model
            first_model_config = self.model_config[0] if self.model_config else None
            if not first_model_config:
                self.logger.warning("Pre-warm skipped: no model config available")
                return

            first_voice = (
                first_model_config.get("voices", [None])[0]
                if first_model_config.get("voices")
                else None
            )
            if not first_voice:
                self.logger.warning("Pre-warm skipped: no voices in default model config")
                return

            model_path = first_voice.get("model_path", "")
            if not model_path:
                self.logger.warning("Pre-warm skipped: model_path not set in default voice config")
                return

            self.logger.info(
                f"Pre-warming TTS engine for default model: {first_model_config.get('name')} "
                f"at {model_path}"
            )
            t_start = time.time()

            async with self.tts_instance_lock:
                if self.tts_instance is not None:
                    # Already initialized (e.g. a request arrived before pre-warm ran)
                    self.logger.info("Pre-warm skipped: TTS instance already exists")
                    return

                instance = self.tts_wrapper_class()
                handle = await asyncio.to_thread(instance.init_dir, model_path)

                if not handle:
                    self.logger.error(
                        f"Pre-warm failed: init_dir returned no handle for {model_path}"
                    )
                    return

                self.tts_instance = instance
                self.current_model_path = model_path

            self.logger.info(
                f"TTS pre-warm complete in {time.time() - t_start:.1f}s — "
                f"first request will reuse this instance"
            )
        except Exception as e:
            self.logger.error(f"Error during TTS pre-warm: {e}", exc_info=True)
    
    
    async def cleanup_tts_resources_after_synthesis(self):
        """Cleanup TTS resources after synthesis when keep_alive=False."""
        self.logger.info('Cleaning up TTS resources after synthesis...')
        
        # Clean up TTS instance
        async with self.tts_instance_lock:
            if self.tts_instance:
                try:
                    self.logger.info("Deinitializing TTS instance (keep_alive=False)...")
                    await asyncio.to_thread(self.tts_instance.deinit)
                    await asyncio.sleep(0.1)
                    self.tts_instance = None
                    self.current_model_path = None
                    self.logger.info("TTS instance deinitialized (keep_alive=False)")
                except Exception as e:
                    self.logger.error(f"Error deinitializing TTS instance: {e}", exc_info=True)
                    self.tts_instance = None
                    self.current_model_path = None
        
        # Clear audio chunks
        self.audio_chunks = []
        
        self.logger.info("TTS resources cleaned up after synthesis")
        await asyncio.sleep(0.5)
        import gc
        gc.collect()
    
    async def cleanup_resources_for_service_switch(self):
        """Cleanup TTS resources when switching to another service."""
        self.logger.info('Cleaning up TTS resources for service switch...')
        
        # Clean up TTS instance if exists (full cleanup for service switch)
        async with self.tts_instance_lock:
            if self.tts_instance:
                try:
                    self.logger.info("Deinitializing TTS instance for service switch...")
                    await asyncio.to_thread(self.tts_instance.deinit)
                    # Wait longer to ensure DSP/NPU resources are fully released
                    # The deinit() call may return before DSP has fully freed resources
                    self.logger.info("Waiting for DSP/NPU resources to be fully released...")
                    await asyncio.sleep(0.5)
                    self.tts_instance = None
                    self.current_model_path = None
                    self.logger.info("TTS instance deinitialized and DSP resources released")
                except Exception as e:
                    self.logger.error(f"Error deinitializing TTS instance: {e}", exc_info=True)
        
        # Clear audio chunks
        self.audio_chunks = []
        
        # Force garbage collection to clean up any lingering references
        # import gc
        # gc.collect()
        self.logger.info("TTS resources released for service switch")
    
    async def cleanup(self):
        """Cleanup TTS resources."""
        self.logger.info('Cleaning up TTS service...')
        
        # Stop the persistent speaker stream
        if self.speaker is not None:
            try:
                self.speaker.stop()
                if self.speaker_thread and self.speaker_thread.is_alive():
                    self.speaker_thread.join(timeout=2.0)
                self.logger.info("Persistent speaker stopped")
            except Exception as e:
                self.logger.error(f"Error stopping speaker: {e}", exc_info=True)
            finally:
                self.speaker = None
                self.speaker_thread = None

        # Clean up TTS instance if exists
        async with self.tts_instance_lock:
            if self.tts_instance:
                try:
                    await asyncio.to_thread(self.tts_instance.deinit)
                    await asyncio.sleep(0.1)
                    self.tts_instance = None
                    self.current_model_path = None
                    self.logger.info("TTS instance cleaned up successfully")
                except Exception as e:
                    self.logger.error(f"Error cleaning up TTS instance: {e}", exc_info=True)
    
    async def _request_queue_worker(self):
        """Process TTS synthesis requests sequentially — one sync_id at a time."""
        self.logger.info("TTS queue worker running")
        while True:
            try:
                message = await self._request_queue.get()
                if message is None:
                    # Shutdown signal
                    self.logger.info("TTS queue worker shutting down")
                    break
                try:
                    await self.handle_synthesize_request(message)
                except Exception as e:
                    self.logger.error(f"Error processing queued TTS request: {e}", exc_info=True)
                finally:
                    self._request_queue.task_done()
            except asyncio.CancelledError:
                self.logger.info("TTS queue worker cancelled")
                break
            except Exception as e:
                self.logger.error(f"Unexpected error in TTS queue worker: {e}", exc_info=True)

    def handle_tts_input(self, message: str):
        """Handle incoming TTS messages — cancel, close, or enqueue for sequential processing."""
        try:
            msg_type = get_message_type(message)
            if msg_type == 'tts_cancel':
                self.handle_cancel_request()
                return
            if msg_type == 'tts_close':
                asyncio.create_task(self.handle_close_request())
                return
            # Any real synthesize request clears the cancellation flag so that
            # subsequent sentences in the same LLM response are not silently dropped.
            self._cancelled = False
            self._cancel_event.clear()
            self._request_queue.put_nowait(message)
            self.logger.info(f"TTS request enqueued (queue depth: {self._request_queue.qsize()})")
        except Exception as e:
            self.logger.error(f'Error handling TTS input: {e}', exc_info=True)
    
    async def handle_synthesize_request(self, message: str):
        """Handle TTS synthesis request."""
        try:
            # If a cancel arrived while this request was sitting in the queue,
            # skip it entirely — don't even parse or start synthesis.
            if self._cancelled:
                self.logger.info('Skipping queued TTS request — cancelled')
                return

            # Parse the request first so keep_alive is set before request_service_start
            # runs — the cleanup callback reads keep_alive, so it must be current
            request = TTSSynthesizeRequest.from_json(message)
            self.coordinator.set_tts_keep_alive(request.keep_alive)

            await self.coordinator.request_service_start(
                'TTS',
                self.cleanup_resources_for_service_switch
            )
            # Reject empty or whitespace-only text — the engine crashes on it
            if not request.text or not request.text.strip():
                self.logger.warning(f'TTS request rejected: empty text')
                complete = TTSComplete(status="done", sync_id=request.sync_id)
                await self.publish(Config.TTS_AUDIO_OUT, complete.to_json())
                return

            self.logger.info(
                f'TTS request: text="{request.text[:80]}...", '
                f'model={request.model}, language={request.language}, keep_alive={request.keep_alive}'
            )
            
            # Validate model
            model_names = [m["name"] for m in self.available_models]

            request_model_voices = []
            for m in self.available_models:
                if m["name"] == request.model:
                    request_model_voices = m["voices"]
                    break

            request_model_languages = {v["language"] for v in request_model_voices}
            if not request.model:
                # If model not specified, use default model
                default_model = next((m["name"] for m in self.available_models), None)
                if default_model:
                    request.model = default_model
                    self.logger.info(f"Using default model: {default_model}")
                else:
                    await self.send_error(
                        Config.TTS_AUDIO_OUT,
                        f'No TTS models available on this server.',
                        sync_id=request.sync_id,
                        param='model'
                    )
                    return
            elif request.model not in model_names:
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    f'TTS model "{request.model}" is not available. '
                    f'Available models: {", ".join(model_names)}',
                    sync_id=request.sync_id,
                    param='model'
                )
                return
            elif request.language not in request_model_languages:
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    f'TTS language, {request.language}, is not supported for given model: "{request.model}". '
                    f'Available languages: {", ".join(language for language in request_model_languages)}',
                    sync_id=request.sync_id,
                    param='language',
                    code='invalid_language_request'
                )
                return

            # Validate voice if specified
            model = next(m for m in self.available_models if m["name"] == request.model)
            if request.voice:
                voice_names = [v["name"] for v in model["voices"]]
                if request.voice not in voice_names:
                    await self.send_error(
                        Config.TTS_AUDIO_OUT,
                        f'Voice "{request.voice}" is not available for model "{request.model}". '
                        f'Available voices: {", ".join(voice_names)}',
                        sync_id=request.sync_id,
                        param='voice'
                    )
                    return
            else:
                # If voice not specified, find a voice for the requested language
                if request.language:
                    matching_voices = [v for v in model["voices"] if v["language"] == request.language]
                    if matching_voices:
                        request.voice = matching_voices[0]["name"]
                        self.logger.info(f"Using voice {request.voice} for language {request.language}")
                    else:
                        # No voice for the requested language
                        supported_languages = list(set([v["language"] for v in model["voices"]]))
                        await self.send_error(
                            Config.TTS_AUDIO_OUT,
                            f'Language "{request.language}" is not supported by model "{request.model}". '
                            f'Supported languages: {", ".join(supported_languages)}',
                            sync_id=request.sync_id,
                            param='language'
                        )
                        return
                else:
                    # No language specified, use default voice
                    if model["voices"]:
                        request.voice = model["voices"][0]["name"]
                        request.language = model["voices"][0]["language"]
                        self.logger.info(f"No language specified, using default voice {request.voice} for language {request.language}")
            
            # Synthesize speech
            await self.synthesize_speech(request)
            
        except Exception as e:
            self.logger.error(f'Error handling synthesize request: {e}', exc_info=True)
            await self.send_error(
                Config.TTS_AUDIO_OUT,
                str(e),
                sync_id=getattr(request, 'sync_id', None) if 'request' in locals() else None
            )
    
    async def synthesize_speech(self, request: TTSSynthesizeRequest):
        """
        Synthesize speech and stream audio chunks.
        
        Args:
            request: TTS synthesis request
        """
        try:
            if self.dev_mode:
                # In development mode, use mock synthesis
                await self.synthesize_speech_mock(request)
            elif not self.tts_wrapper_class:
                # Engine failed to initialize at startup
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    'TTS engine is not available. The engine failed to initialize at startup.',
                    sync_id=request.sync_id
                )
            else:
                # In production mode, use the actual TTS engine
                await self.synthesize_speech_real(request)
                
        except Exception as e:
            self.logger.error(f'Error synthesizing speech: {e}', exc_info=True)
            raise
    
    async def synthesize_speech_mock(self, request: TTSSynthesizeRequest):
        """Generate mock speech synthesis for development mode by reading MobyDick WAV file."""
        self.logger.info('Synthesizing speech (mock)...')
        
        try:
            # Path to the mock WAV file
            mock_file_path = os.path.join(
                os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                'mock', 'MobyDick5seconds19Words.wav'
            )
            
            if not os.path.exists(mock_file_path):
                self.logger.warning(f"Mock WAV file not found at {mock_file_path}, using generated silence")
                # Fall back to generating silence
                await self.synthesize_speech_mock_fallback(request)
                return
            
            self.logger.info(f"Reading mock WAV file from {mock_file_path}")
            
            # Read and parse the WAV file to extract raw PCM
            with wave.open(mock_file_path, 'rb') as wav_file:
                # Get WAV file parameters
                n_channels = wav_file.getnchannels()
                sampwidth = wav_file.getsampwidth()
                framerate = wav_file.getframerate()
                n_frames = wav_file.getnframes()
                
                self.logger.info(f"WAV file: {n_channels} channels, {sampwidth} bytes/sample, {framerate} Hz, {n_frames} frames")
                
                # Read raw PCM data (strips WAV header)
                raw_pcm = wav_file.readframes(n_frames)
            
            # Get requested sample rate (default to 44100 if not specified)
            target_sample_rate = request.sample_rate or 44100
            
            # Resample if needed
            if framerate != target_sample_rate or n_channels != 1:
                self.logger.info(f"Resampling from {framerate}Hz {n_channels}ch to {target_sample_rate}Hz mono")
                from common.resampler import resample_audio
                
                raw_pcm = resample_audio(
                    raw_pcm,
                    original_sample_rate=framerate,
                    target_sample_rate=target_sample_rate,
                    original_channels=n_channels,
                    target_channels=1,  # Always mono
                    sample_width=sampwidth,
                    is_wav_file=False  # Already raw PCM
                )
                self.logger.info(f"Resampled to {len(raw_pcm)} bytes")
            
            # Split into chunks for streaming
            chunk_size = 8192  # 8KB chunks
            total_size = len(raw_pcm)
            num_chunks = (total_size + chunk_size - 1) // chunk_size
            
            self.logger.info(f"Sending {total_size} bytes of raw PCM in {num_chunks} chunks at {target_sample_rate}Hz")
            
            for i in range(num_chunks):
                start = i * chunk_size
                end = min(start + chunk_size, total_size)
                chunk = raw_pcm[start:end]
                
                # Send base64-encoded audio chunk wrapped with sync_id so the
                # API handler can filter out chunks from other requests.
                import json as _json
                chunk_msg = _json.dumps({
                    "type": "audio",
                    "data": base64.b64encode(chunk).decode('ascii'),
                    "sync_id": request.sync_id
                })
                await self.publish(Config.TTS_AUDIO_OUT, chunk_msg)
                self.logger.debug(f'Sent mock audio chunk {i+1}/{num_chunks} ({len(chunk)} bytes)')
                
                # Small delay between chunks to simulate streaming
                await asyncio.sleep(0.05)
            
            # Send completion message
            complete = TTSComplete(
                status="done",
                sync_id=request.sync_id
            )
            
            await self.publish(Config.TTS_AUDIO_OUT, complete.to_json())
            self.logger.info('Mock TTS synthesis complete')
            
            # Clean up resources if keep_alive is False
            if not request.keep_alive:
                await self.cleanup_tts_resources_after_synthesis()
                self.logger.info('TTS resources cleaned up (keep_alive=False)')
            
        except Exception as e:
            self.logger.error(f"Error reading mock WAV file: {e}", exc_info=True)
            # Fall back to generating silence
            await self.synthesize_speech_mock_fallback(request)
    
    async def synthesize_speech_mock_fallback(self, request: TTSSynthesizeRequest):
        """Fallback mock synthesis using generated silence."""
        self.logger.info('Using fallback mock synthesis (silence)...')
        
        # Simulate generating audio chunks
        num_chunks = 5
        for i in range(num_chunks):
            # Generate mock audio chunk
            audio_chunk = self.generate_mock_audio_chunk(i)
            
            # Send base64-encoded audio chunk wrapped with sync_id
            import json as _json
            chunk_msg = _json.dumps({
                "type": "audio",
                "data": base64.b64encode(audio_chunk).decode('ascii'),
                "sync_id": request.sync_id
            })
            await self.publish(Config.TTS_AUDIO_OUT, chunk_msg)
            self.logger.debug(f'Sent fallback mock audio chunk {i+1}/{num_chunks}')
            
            # Simulate processing time
            await asyncio.sleep(0.2)
        
        # Send completion message
        complete = TTSComplete(
            status="done",
            sync_id=request.sync_id
        )
        
        await self.publish(Config.TTS_AUDIO_OUT, complete.to_json())
        self.logger.info('Fallback mock TTS synthesis complete')
        
        # Clean up resources if keep_alive is False
        if not request.keep_alive:
            await self.cleanup_tts_resources_after_synthesis()
            self.logger.info('TTS resources cleaned up (keep_alive=False)')
    
    async def synthesize_speech_real(self, request: TTSSynthesizeRequest):
        """Use the actual TTS engine to synthesize speech."""
        self.logger.info('Synthesizing speech...')
        
        # Clear any previous audio chunks
        self.audio_chunks = []
        
        try:
            # Find the model and voice configuration
            model_config = next((m for m in self.model_config if m["name"] == request.model), None)
            if not model_config:
                self.logger.error(f"Model configuration not found for {request.model}")
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    f'Internal error: model configuration not found for "{request.model}".',
                    sync_id=request.sync_id
                )
                return
            
            # Find the voice configuration
            voice_config = None
            if request.voice:
                voice_config = next((v for v in model_config.get("voices", []) if v["name"] == request.voice), None)
            elif request.language:
                voice_config = next((v for v in model_config.get("voices", []) if v["language"] == request.language), None)
            else:
                # Use the first voice
                if model_config.get("voices"):
                    voice_config = model_config["voices"][0]
            
            if not voice_config:
                self.logger.error(f"Voice configuration not found for {request.voice or request.language}")
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    f'Internal error: voice configuration not found for voice "{request.voice or request.language}".',
                    sync_id=request.sync_id
                )
                return
            
            # Get the model path
            model_path = voice_config.get("model_path", "")
            if not model_path:
                self.logger.error(f"Model path not found for voice {voice_config['name']}")
                await self.send_error(
                    Config.TTS_AUDIO_OUT,
                    f'Internal error: model path not configured for voice "{voice_config["name"]}".',
                    sync_id=request.sync_id
                )
                return
            
            # Get TTS parameters from request or use defaults
            speaking_rate = None
            pitch = None
            volume_gain = None
            sample_rate = request.sample_rate or 44100
            output_speaker = request.output_speaker
            on_device_playback = request.on_device_playback 
            output_speaker_name = request.output_speaker_name

            # Parse parameters if provided
            if request.parameters:
                speaking_rate = request.parameters.get("speaking_rate")
                pitch = request.parameters.get("pitch")
                volume_gain = request.parameters.get("gain")
            
            # Get the appropriate language code map based on model type
            language_code_map = self.TTS_LANGUAGE_CODE_MAPPING[model_config["model_type"]]
            language_code = language_code_map.get(request.language.lower() if request.language else "", 0)
            
            tts_param = {}
            if speaking_rate is not None:
                tts_param["speaking_rate"] = float(speaking_rate)
            if pitch is not None:
                tts_param["pitch"] = float(pitch)
            if volume_gain is not None:
                tts_param["volume_gain"] = float(volume_gain)
            # What else?

            def is_matching_prev_tts_config(config):
                return tts_param == self._prev_tts_param

            # Get or create singleton TTS instance (only reinit if model changed)
            async with self.tts_instance_lock:
                if self.tts_instance is None or self.current_model_path != model_path or not is_matching_prev_tts_config(tts_param):
                    self.logger.info(
                        f"TTS instance state: instance={'exists' if self.tts_instance else 'None'}, "
                        f"current_path={self.current_model_path}, requested_path={model_path}, "
                        f"paths_match={self.current_model_path == model_path}"
                    )
                    # Need to create or recreate instance
                    if self.tts_instance is not None:
                        self.logger.info(f"Model changed from {self.current_model_path} to {model_path}, reinitializing TTS...")
                        try:
                            await asyncio.to_thread(self.tts_instance.deinit)
                            await asyncio.sleep(0.1)
                        except Exception as e:
                            self.logger.error(f"Error deinitializing old TTS instance: {e}")
                        self.tts_instance = None
                    
                    self.logger.info(f"Creating new TTS instance for model: {model_path}")
                    self.tts_instance = self.tts_wrapper_class()

                    # Set the previous tts param
                    self._prev_tts_param = tts_param

                    # Initialize the TTS engine with the model directory
                    t_init = time.time()
                    handle = await asyncio.to_thread(
                        self.tts_instance.init_dir,
                        model_path,
                        tts_param,
                    )
                    self.logger.info(
                        f"TTS init_dir completed in {time.time() - t_init:.1f}s "
                        f"for model: {model_path}"
                    )
                    
                    if not handle:
                        self.logger.error("Failed to initialize TTS engine")
                        self.tts_instance = None
                        self.current_model_path = None
                        await self.send_error(
                            Config.TTS_AUDIO_OUT,
                            f'TTS engine failed to initialize for model "{request.model}".',
                            sync_id=request.sync_id
                        )
                        return
                    
                    self.current_model_path = model_path
                    self.logger.info(f"TTS engine initialized with handle: {handle}")
                else:
                    self.logger.info(f"Reusing existing TTS instance for model: {model_path}")
                    # Note: tts_configs overrides only apply when the engine is
                    # (re)initialised.
                
                # Use the singleton instance
                tts_instance = self.tts_instance
            
            # Each model has its own required sample rates
            model_sample_rate = {
                "melo" : 44100,
                "piper": 22050
            }

            # TTS engine sample rate based on model. Melo is 44100, Piper is 22050
            engine_sample_rate = model_sample_rate[model_config["model_type"]]
            engine_channels = 1
            
            # Get requested sample rate (default to 44100 if not specified)
            target_sample_rate = request.sample_rate or engine_sample_rate
            logger.info(f"Target Sample Rate: {target_sample_rate}")

            # Create a thread-safe queue for chunks
            chunk_queue = queue.Queue()
            # Process the text with callback
            text = request.text

            # Use the persistent speaker if on-device playback is requested.
            # Always call _start_persistent_speaker so a device change takes effect;
            # it is a no-op when the same device is already running.
            if output_speaker or on_device_playback:
                self._start_persistent_speaker(output_speaker_name)
                speaker = self.speaker
                # Record the time this synthesis started so the idle-timeout
                # logic in _start_persistent_speaker can detect stale streams.
                self.speaker_last_used = time.time()
                if speaker is None:
                    self.logger.error("Speaker init failed — falling back to stream")
                    on_device_playback = False
            else:
                speaker = None
            self.logger.info(
                f"Playback mode: on_device={on_device_playback}, output_speaker={output_speaker}, "
                f"speaker_available={speaker is not None}, "
                f"speaker_thread_alive={self.speaker_thread.is_alive() if self.speaker_thread else False}, "
                f"output_speaker_name={output_speaker_name},"
                f"sync_id={request.sync_id}"
            )

            # Pre-compute resample parameters once so the callback does no redundant work
            speaker_sample_rate = self.speaker.cached_sample_rate if self.speaker else None
            speaker_channels = self.speaker.cached_channels if self.speaker else None
            if speaker_sample_rate is None: speaker_sample_rate = 44100
            if speaker_channels is None: speaker_channels = 2
            need_stream_resample = engine_sample_rate != target_sample_rate
            need_speaker_resample = on_device_playback and (
                engine_sample_rate != speaker_sample_rate or
                engine_channels != speaker_channels
            )

            chunk_count_ref = [0]  # mutable counter accessible inside callback

            @self.tts_wrapper_class.CHUNK_CALLBACK_TYPE
            def chunk_callback(pcm_data, pcm_size):
                """Callback function to receive audio chunks from TTS engine."""
                try:
                    chunk = ctypes.string_at(pcm_data, pcm_size)
                    chunk_count_ref[0] += 1
                    self.logger.info(f"TTS chunk {chunk_count_ref[0]}: {pcm_size} bytes, on_device={on_device_playback}")

                    if on_device_playback:
                        # Playing on device — send to speaker only, skip Redis stream
                        try:
                            if need_speaker_resample:
                                device_chunk = resample_audio(
                                    chunk,
                                    original_sample_rate=engine_sample_rate,
                                    target_sample_rate=speaker_sample_rate,
                                    original_channels=engine_channels,
                                    target_channels=speaker_channels,
                                    sample_width=2,
                                    is_wav_file=False
                                )
                            else:
                                device_chunk = chunk
                            speaker.add_to_buffer(device_chunk)
                        except Exception as e:
                            self.logger.error(f"Error adding chunk to speaker buffer: {e}", exc_info=True)
                    else:
                        # Streaming back to client — resample for target rate and queue
                        processed_chunk = resample_audio(
                            chunk,
                            original_sample_rate=engine_sample_rate,
                            target_sample_rate=target_sample_rate,
                            original_channels=engine_channels,
                            target_channels=1,
                            sample_width=2,
                            is_wav_file=False
                        ) if need_stream_resample else chunk
                        #self.logger.info("adding to chunk_queue")
                        chunk_queue.put(processed_chunk)

                except Exception as e:
                    self.logger.error(f"Error in chunk callback: {e}", exc_info=True)
                    if not on_device_playback:
                        chunk_queue.put(None)  # Signal error to stream consumer

            # Start processing in a separate thread
            def process_tts():
                try:
                    text_index = 0
                    batch_number = 0
                    while text_index < len(text):
                        chunked_text = text[text_index: text_index + self.tts_max_char_len - 1]
                        result = tts_instance.process_with_callback(chunked_text, chunk_callback)
                        if result != 0:
                            self.logger.error(f"TTS process failed with code {result}")
                        else:
                            self.logger.info(f"TTS processed batch #{batch_number} of text from {text_index} to {text_index + len(chunked_text)}")
                        text_index += len(chunked_text)
                        batch_number += 1
                finally:
                    chunk_queue.put(None)  # Signal completion (only used in stream mode)

            # Start TTS processing in background thread
            tts_thread = threading.Thread(target=process_tts)
            tts_thread.start()

            if on_device_playback:
                # Wait for TTS thread to finish — persistent speaker plays directly, nothing to stream.
                # Poll in short intervals so a cancel can interrupt the wait.
                while tts_thread.is_alive():
                    if self._cancelled:
                        self.logger.info('Cancel detected during on-device playback — clearing speaker buffer')
                        if self.speaker is not None:
                            self.speaker.clear_buffer()
                        break
                    await asyncio.sleep(0.05)
                tts_thread.join(timeout=1.0)
                self.logger.info(f"TTS processing complete (on-device playback, {chunk_count_ref[0]} chunks fed to speaker)")
                # Speaker keeps running — no mark_buffer_finished needed
            else:
                # Stream chunks back to client via Redis or Socket
                chunk_count = 0
                while True:
                    # Check cancellation before blocking on the queue
                    if self._cancelled:
                        self.logger.info('Cancel detected during stream — aborting chunk loop')
                        break
                    try:
                        # Use a short timeout so the cancel flag is polled regularly
                        chunk = await asyncio.to_thread(chunk_queue.get, timeout=0.1)
                        if chunk is None:  # Completion signal
                            break
                        chunk_count += 1
                        self.logger.info(f"Processing chunk {chunk_count}: {len(chunk)} bytes")
                        import json as _json
                        chunk_msg = _json.dumps({
                            "type": "audio",
                            "data": base64.b64encode(chunk).decode('ascii'),
                            "sync_id": request.sync_id
                        })
                        await self.publish(Config.TTS_AUDIO_OUT, chunk_msg)
                    except queue.Empty:
                        # Timed out waiting — loop back and re-check cancel flag
                        if not tts_thread.is_alive():
                            # TTS thread finished but sent no None sentinel — drain remaining
                            try:
                                while True:
                                    chunk = chunk_queue.get_nowait()
                                    if chunk is None:
                                        break
                                    chunk_count += 1
                                    import json as _json
                                    chunk_msg = _json.dumps({
                                        "type": "audio",
                                        "data": base64.b64encode(chunk).decode('ascii'),
                                        "sync_id": request.sync_id
                                    })
                                    await self.publish(Config.TTS_AUDIO_OUT, chunk_msg)
                            except queue.Empty:
                                pass
                            break
                        continue
                    except Exception as e:
                        self.logger.error(f"Error processing chunk: {e}", exc_info=True)
                        break

                await asyncio.to_thread(tts_thread.join, timeout=1.0)
                self.logger.info(f"TTS processing complete, sent {chunk_count} chunks")

            self.logger.info("TTS processing complete, keeping singleton instance alive for reuse")
            
            # Send completion message
            complete = TTSComplete(
                status="done",
                sync_id=request.sync_id
            )
            await self.publish(Config.TTS_AUDIO_OUT, complete.to_json())
            self.logger.info('TTS synthesis complete')
            
            # Clean up resources if keep_alive is False
            if not request.keep_alive:
                await self.cleanup_tts_resources_after_synthesis()
                self.logger.info('TTS resources cleaned up (keep_alive=False)')
                        
        except Exception as e:
            self.logger.error(f"Error using TTS engine: {e}", exc_info=True)
            await self.send_error(
                Config.TTS_AUDIO_OUT,
                f'TTS engine error: {e}',
                sync_id=request.sync_id
            )
    
    def generate_mock_audio_chunk(self, chunk_index: int) -> bytes:
        """
        Generate a mock audio chunk for testing.
        
        Args:
            chunk_index: Index of the chunk
            
        Returns:
            Mock audio bytes
        """
        # TODO: Remove this when real TTS engine is integrated
        # Generate some dummy audio data (silence)
        sample_rate = 22050
        duration = 0.2  # 200ms
        num_samples = int(sample_rate * duration)
        
        # Simple silence (16-bit PCM)
        audio_data = b'\x00\x00' * num_samples
        return audio_data
    
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
            
            request = TTSModelsRequest.from_json(message)
            
            # Filter by language if specified
            models = self.available_models
            if request.language:
                models = []
                for model in self.available_models:
                    # Filter voices by language
                    filtered_voices = [
                        v for v in model["voices"]
                        if v["language"] == request.language
                    ]
                    if filtered_voices:
                        filtered_model = model.copy()
                        filtered_model["voices"] = filtered_voices
                        models.append(filtered_model)
            
            # Send models list — result is the plain models array
            response = {
                "sync_id": request.sync_id,
                "result": models,
                "message_source": "audio_analytics_server"
            }
            
            asyncio.create_task(
                self.publish(Config.TTS_MODELS, json.dumps(response))
            )
            
            self.logger.info(f'Sent models list: {len(models)} models')

        except Exception as e:
            self.logger.error(f'Error handling models request: {e}', exc_info=True)

    def handle_devices_request(self, message: str):
        """Handle playback devices list request."""
        try:
            import json
            data = json.loads(message)
            message_source = data.get('message_source', '')

            # Ignore messages from ourselves (responses)
            if message_source == 'audio_analytics_server':
                self.logger.debug('Ignoring message from server (our own response)')
                return

            sync_id = data.get('sync_id', '')

            # Enumerate available audio output (playback) devices.
            # The persistent speaker is almost always running (started lazily on
            # the first on_device_playback request), so we cannot simply pass
            # refresh=True — that would call sd._terminate() and kill the live
            # stream.  Instead we:
            #   1. Signal the speaker thread to stop (sets _stop_event).
            #   2. Join the thread so the stream is fully closed.
            #   3. Call list_output_devices(refresh=True) for a fresh list.
            #   4. Restart the speaker on the same device via
            #      _start_persistent_speaker so it is ready for the next
            #      on_device_playback request.
            output_devices = []
            try:
                self.logger.info("=== Listing available audio output devices (devices request) ===")
                from utils.speaker import Speaker

                # --- stop speaker so sd._terminate() is safe ---
                speaker_was_running = self.speaker is not None and self.speaker.is_running()
                saved_device_name = self.speaker_device_name  # may be None
                if speaker_was_running:
                    self.logger.info("Temporarily stopping speaker for device-list refresh")
                    try:
                        self.speaker._stop_event.set()
                        if self.speaker_thread and self.speaker_thread.is_alive():
                            self.speaker_thread.join(timeout=2.0)
                    except Exception as stop_err:
                        self.logger.warning(f"Error stopping speaker before refresh: {stop_err}")

                available_devices = Speaker.list_output_devices(refresh=True)

                # --- restart speaker if it was running ---
                if speaker_was_running and saved_device_name:
                    self.logger.info(f"Restarting speaker on '{saved_device_name}' after refresh")
                    try:
                        self._start_persistent_speaker(saved_device_name)
                    except Exception as restart_err:
                        self.logger.warning(f"Error restarting speaker after refresh: {restart_err}")

                for dev in available_devices:
                    self.logger.info(
                        f"  - {dev['index']}: {dev['name']} "
                        f"({dev['default_samplerate']} Hz, {dev['max_output_channels']} channels)"
                    )
                    output_devices.append({
                        "index": dev["index"],
                        "name": dev["name"],
                        "default_samplerate": dev["default_samplerate"],
                        "max_output_channels": dev["max_output_channels"],
                    })
            except Exception as dev_err:
                self.logger.error(f"Error listing output devices: {dev_err}")

            response = {
                "sync_id": sync_id,
                "result": output_devices,
                "message_source": "audio_analytics_server"
            }

            asyncio.create_task(
                self.publish(Config.TTS_DEVICES, json.dumps(response))
            )

            self.logger.info(f'Sent playback devices list: {len(output_devices)} devices')

        except Exception as e:
            self.logger.error(f'Error handling devices request: {e}', exc_info=True)
