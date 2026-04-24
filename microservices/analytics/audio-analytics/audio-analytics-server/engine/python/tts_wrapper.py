# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TTS Wrapper with Global Callback Management

This version uses a global callback manager to ensure:
1. C function pointers remain valid (no dangling pointers)
2. Callbacks can be dynamically updated without re-registering with C
3. Multiple wrapper instances can coexist with different callbacks
4. Library is loaded only once for efficiency
"""

import ctypes
import pathlib
import base64
import wave
import socket
import os
import sys
import time
import threading

import wrapper_utils as wu
from tts_model_registry import get_model_config, is_model_registered, list_registered_models

# Environment variable for model storage location
TTS_MODEL_STORE_DIR = os.environ.get('TTS_MODEL_STORE_DIR', '/tmp/audio-cache')

# ----------------------------------------------------------------------
# Callback type definition
# ----------------------------------------------------------------------
CHUNK_CALLBACK_TYPE = ctypes.CFUNCTYPE(
    None,                           # return type (void)
    ctypes.POINTER(ctypes.c_uint8), # pcm_data pointer
    ctypes.c_uint32,                # pcm_size
)


# ----------------------------------------------------------------------
# Global Callback Manager for TTS
# ----------------------------------------------------------------------
class TTSCallbackManager:
    """Manages global TTS callback routing with strong references.
    
    This ensures that:
    1. C function pointers remain valid (no dangling pointers)
    2. Callbacks can be dynamically updated without re-registering with C
    3. Multiple wrapper instances can coexist with different callbacks
    """
    
    def __init__(self):
        # Strong reference to C callback function (never garbage collected)
        self._c_chunk_callback = None
        
        # Current Python callback handler (can be updated)
        self._current_chunk_handler = None
        
        # Lock for thread-safe callback updates
        self._lock = threading.Lock()
        
        # Initialize the global C callback once
        self._initialize_global_callback()
    
    def _initialize_global_callback(self):
        """Create global C callback function that routes to current handler."""
        
        @CHUNK_CALLBACK_TYPE
        def _global_chunk_callback(pcm_data, pcm_size):
            """Global C callback that routes to the current chunk handler."""
            with self._lock:
                handler = self._current_chunk_handler
            
            if handler:
                try:
                    # Pass the raw pointer directly - don't convert to bytes
                    handler(pcm_data, pcm_size)
                except Exception as e:
                    print(f"Error in global TTS chunk callback: {e}")
                    import traceback
                    traceback.print_exc()
        
        # Store strong reference to prevent garbage collection
        self._c_chunk_callback = _global_chunk_callback
        
        print("Global TTS callback initialized")
    
    def set_handler(self, on_chunk=None):
        """Update the current chunk callback handler.
        
        Args:
            on_chunk: Python function to handle audio chunks (chunk_bytes, chunk_size)
        """
        with self._lock:
            if on_chunk is not None:
                self._current_chunk_handler = on_chunk
        
        print(f"Updated TTS chunk callback handler: {on_chunk is not None}")
    
    def clear_handler(self):
        """Clear the current handler (but keep C callback alive)."""
        with self._lock:
            self._current_chunk_handler = None
        
        print("Cleared TTS chunk callback handler")
    
    def get_c_callback(self):
        """Get the global C callback function pointer.
        
        Returns:
            C callback function pointer
        """
        return self._c_chunk_callback


# Global singleton instance
_global_tts_callback_manager = TTSCallbackManager()


class TTS:
    """TTS wrapper with global callback management and optimized library loading.
    
    Example:
        tts = TTS()
        tts.init(b"/path/to/model.qnn", sample_rate=44100, language_code=1)
        tts.process_to_file(b"Hello world", b"output.wav")
        tts.deinit()
    """
    
    # Class-level callback type for backward compatibility
    CHUNK_CALLBACK_TYPE = CHUNK_CALLBACK_TYPE
    
    # Class-level variables (shared across all instances)
    _shared_lib = None
    _library_loaded = False

    # Background cache writer thread and the cache path it is writing to —
    # tracked so init_dir can wait for it only when the same model is requested
    # again before the write has finished.
    _cache_writer_thread: threading.Thread = None
    _cache_writer_path: str = None
    _cache_writer_cancel: threading.Event = threading.Event()

    def __init__(self, lib_path=None):
        """Initialize TTS wrapper.
        
        Args:
            lib_path: Optional path to libtts_c_wrapper.so
        """
        self.handle = None
        self.audio_buffer = []  # For storing audio chunks when using callbacks
        
        # Load the C library only once (class-level)
        if not TTS._library_loaded:
            TTS._shared_lib = self._load_library(lib_path)
            self._define_ctypes(TTS._shared_lib)
            TTS._library_loaded = True
            print("TTS library loaded for the first time")
        else:
            print("Reusing already loaded TTS library")
        
        # Use the shared library instance
        self.c_lib = TTS._shared_lib

    def _load_library(self, lib_path=None):
        """Load the TTS shared library.
        
        Args:
            lib_path: Optional path to libtts_c_wrapper.so
            
        Returns:
            Loaded ctypes.CDLL library object
        """
        if lib_path is None:
            lib_dir = os.path.abspath(os.path.dirname(__file__))
            primary_path = os.path.join(lib_dir, "libtts_c_wrapper.so")
            server_path = "/usr/src/server/libtts_c_wrapper.so"
            fallback_path = "/usr/lib/libtts_c_wrapper.so"

            if os.path.isfile(primary_path):
                lib_path = primary_path
                print("Using primary TTS library path")
            elif os.path.isfile(server_path):
                lib_path = server_path
                print("Using server TTS library path")
            elif os.path.isfile(fallback_path):
                lib_path = fallback_path
                print("Using fallback TTS library path")
            else:
                sys.stderr.write(
                    f"Error: libtts_c_wrapper.so not found at {primary_path}, {server_path}, or {fallback_path}\n"
                )
                raise FileNotFoundError("libtts_c_wrapper.so not found")
        else:
            if not os.path.isfile(lib_path):
                sys.stderr.write(f"Error: {lib_path} not found\n")
                raise FileNotFoundError(lib_path)

        print(f"Loading TTS library from: {lib_path}")
        return ctypes.CDLL(lib_path)

    class TTSParams(ctypes.Structure):
        """C struct for TTS parameters matching tts_params_t in C."""
        _fields_ = [
            ("audio_encoding_code", ctypes.c_uint32),
            ("speaking_rate", ctypes.c_float),
            ("pitch", ctypes.c_float),
            ("volume_gain", ctypes.c_float),
            ("sample_rate", ctypes.c_uint32),
            ("language_code", ctypes.c_uint32),
        ]

    def _define_init_model_file_ctype(self, c_lib):
        """Define C types for the init_model_file function."""
        c_lib.init_model_file.argtypes = [
            ctypes.c_char_p,  # model_file
            self.TTSParams    # tts_params
        ]
        c_lib.init_model_file.restype = ctypes.c_uint64

    def _define_init_model_buffer_ctype(self, c_lib):
        """Define C types for the init_model_buffer function."""
        c_lib.init_model_buffer.argtypes = [
            ctypes.c_char_p,  # model_buffer
            ctypes.c_int,     # model_buffer_size
            self.TTSParams    # tts_params
        ]
        c_lib.init_model_buffer.restype = ctypes.c_uint64

    def _define_process_ctype(self, c_lib):
        """Define C types for the process function."""
        c_lib.process.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,  # text
            ctypes.c_void_p,  # callback
            ctypes.c_char_p   # output_file
        ]
        c_lib.process.restype = ctypes.c_int

    def _define_deinit_ctype(self, c_lib):
        """Define C types for the deinit function."""
        c_lib.deinit.argtypes = [ctypes.c_uint64]
        c_lib.deinit.restype = ctypes.c_int

    def _define_ctypes(self, c_lib):
        """Define all C types for the library."""
        self._define_init_model_file_ctype(c_lib)
        self._define_init_model_buffer_ctype(c_lib)
        self._define_process_ctype(c_lib)
        self._define_deinit_ctype(c_lib)

    @staticmethod
    def _cleanup_cache_dir(cache_dir: str):
        """Remove all .qnn and .qnn.tmp files from the cache directory."""
        try:
            for filename in os.listdir(cache_dir):
                if filename.endswith('.qnn') or filename.endswith('.qnn.tmp'):
                    file_path = os.path.join(cache_dir, filename)
                    try:
                        os.remove(file_path)
                        print(f"[cache_writer] Removed old cache file: {file_path}")
                    except Exception as e:
                        print(f"[cache_writer] Could not remove {file_path}: {e}")
        except FileNotFoundError:
            pass
        except Exception as e:
            print(f"[cache_writer] Error during cache cleanup: {e}")

    @staticmethod
    def _write_cache_files_bg(model_buffer, source_cache_location,
                              tmp_cache_location, store_location_dir,
                              cancel_event: threading.Event, model_type):
        """Write packed .qnn cache files in a background thread."""
        # Get the appropriate generator based on model type
        try:
            model_config = get_model_config(model_type)
            generate_packed_model_file = model_config.generate_packed_file_func
        except ValueError as e:
            print(f"[cache_writer] Error: {e}")
            return
        
        # ── Write persistent cache (alongside model assets) ───────────────────
        if cancel_event.is_set():
            print(f"[cache_writer] Cancelled before persistent cache write")
            return
        try:
            if os.path.exists(source_cache_location):
                os.remove(source_cache_location)
            t = time.time()
            generate_packed_model_file(source_cache_location, model_buffer)
            print(f"[cache_writer] Wrote persistent cache to {source_cache_location} "
                  f"in {time.time() - t:.2f}s")
        except OSError as e:
            print(f"[cache_writer] Could not write persistent cache to "
                  f"{source_cache_location}: {e} — skipping")

        # ── Write /tmp cache ──────────────────────────────────────────────────
        if cancel_event.is_set():
            print(f"[cache_writer] Cancelled before /tmp cache write")
            return
        try:
            if not os.path.isdir(store_location_dir):
                os.makedirs(store_location_dir)
            if os.path.exists(tmp_cache_location):
                os.remove(tmp_cache_location)
            t = time.time()
            generate_packed_model_file(tmp_cache_location, model_buffer)
            print(f"[cache_writer] Wrote /tmp cache to {tmp_cache_location} "
                  f"in {time.time() - t:.2f}s")
        except OSError as e:
            print(f"[cache_writer] Could not write /tmp cache to "
                  f"{tmp_cache_location}: {e}")
        finally:
            # Release the buffer reference as soon as we are done so the
            # calling thread's gc.collect() can reclaim it promptly.
            model_buffer = None

    def init_dir(self, model_dir_location):
        """
        Initialize the TTS engine with path to model files instead of generated blob.

        Cache strategy (fastest → slowest):
          1. Packed .qnn file co-located with the model source directory
             (<model_dir>/<name>.qnn) — survives container restarts, no /tmp
             dependency.
          2. Packed .qnn file in TTS_MODEL_STORE_DIR (/tmp/tts by default) —
             written on first run, reused on subsequent runs within the same
             container lifetime.
          3. Full generate_model() + init_model_buffer() — only runs when
             neither cache location has a valid file.  Cache files are written
             in a background daemon thread so they never block the caller.
        """
        t_start = time.time()

        # Validate model directory location
        if not os.path.isdir(model_dir_location):
            print(f"{model_dir_location} is not a valid directory")
            return

        print(f"Processing model directory: {model_dir_location}")

        # Load config.json and match files to assets
        config_json = None
        try:
            config_path = wu.search_file("config.json", model_dir_location)
            print(f"Found config.json at: {config_path}")
            config_json = wu.open_file(config_path)
            print(f"Successfully loaded config.json with keys: {list(config_json.keys())}")
            
            # Check if there's a pre-built model file specified in the config
            if "model_file" in config_json:
                try:
                    model_file = config_json["model_file"]
                    model_file_path = os.path.join(model_dir_location, model_file)
                    print(f"Checking for model file at: {model_file_path}")
                    if os.path.isfile(model_file_path):
                        print(f"Found pre-built model file in config: {model_file_path}")
                        # Get voices from config
                        voices = config_json["voices"]
                        # Use the pre-built model file directly
                        return self.init_model(
                            model_file_path,
                            audio_encoding=voices[0]["audio_encoding"],
                            speaking_rate=voices[0]["speaking_rate"],
                            pitch=voices[0]["pitch"],
                            volume_gain=voices[0]["volume_gain"],
                            sample_rate=voices[0]["sample_rate"],
                            language_code=voices[0]["language_code"]
                        )
                    else:
                        print(f"Model file specified in config not found: {model_file_path}")
                except Exception as e:
                    print(f"Error using model_file from config: {e}")
                    # Continue with normal initialization
            else:
                print("No model_file specified in config.json, continuing with normal initialization")
            
            # If we get here, either there was no model_file or it couldn't be loaded
            # Continue with normal initialization
            print("Matching files to assets from config.json")
            if config_json is None:
                print("Error: config_json is None, cannot continue with initialization")
                return None
                
            models_dict = wu.match_files_to_assets(config_json, model_dir_location)
            print(f"Matched files to assets: {models_dict}")
        except Exception as e:
            print(f"Error loading config.json: {e}")
            import traceback
            traceback.print_exc()
            return  # If we can't load the config, we can't continue
        

        # Check for required model files
        for model, path in models_dict.items():
            # If path is None, it means we do not need that model file
            # Otherwise, we check for the existence of it
            if path and not os.path.isfile(path):
                print(f"No {model} found in {model_dir_location}")
                return

        generic_model_files = {
            # Generic keys (new config format for models down the line)
            "model",
            "tokenizer",
            "normalizer",
            "encoder",
            "flow",
            "decoder",
            "sdp",

            # Common keys (used by both Melo, Piper, and future models)
            "g2p_encoder",
            "g2p_decoder"
        }

        legacy_melo_model_files = {
            # Melo-specific keys (legacy format)
            "bert_model",
            "bert_tokenizer",
            "bert_normalizer",
            "melo_encoder",
            "melo_flow",
            "melo_decoder",
            "sdp_model",
            
            # Common keys (used by both Melo, Piper, and future models)
            "g2p_encoder",
            "g2p_decoder"
        }

        # Detect model type from config or directory structure
        model_type = config_json.get("model_type", "melo").lower()

        model_files = legacy_melo_model_files if model_type == "melo" else generic_model_files

        # Some files are not required for the models. If they are not in the dir, it is not needed
        for model_file in model_files:
            if model_file not in models_dict:
                models_dict[model_file] = None

        print(f"Models Dict: {models_dict}")

        voices = config_json["voices"]

        # Use default voice for now
        tts_params = self.TTSParams(
            audio_encoding_code=voices[0]["audio_encoding"],
            speaking_rate=voices[0]["speaking_rate"],
            pitch=voices[0]["pitch"],
            volume_gain=voices[0]["volume_gain"],
            sample_rate=voices[0]["sample_rate"],
            language_code=voices[0]["language_code"]
        )

        runtimes = config_json["runtime"]

        model_name = f"{config_json['name']}.qnn"

        # ── Cache location 1: alongside the model source directory ────────────
        # This path survives container restarts because it lives on the same
        # persistent volume as the model assets themselves.
        source_cache_location = os.path.join(model_dir_location, model_name)

        # ── Cache location 2: TTS_MODEL_STORE_DIR (/tmp/tts) ─────────────────
        store_location_dir = TTS_MODEL_STORE_DIR
        tmp_cache_location = os.path.join(store_location_dir, model_name)

        # ── Cancel or wait for any in-progress cache write ────────────────────
        # Different model → cancel immediately so the old model_buffer is
        # released before we allocate a new one (prevents OOM on low-memory
        # devices where two large buffers cannot coexist).
        # Same model → wait so we can use the cache it's writing.
        t = TTS._cache_writer_thread
        if t is not None and t.is_alive():
            if TTS._cache_writer_path != tmp_cache_location:
                print(f"[init_dir] Cancelling cache writer for "
                      f"{TTS._cache_writer_path} (new model: {tmp_cache_location})")
                TTS._cache_writer_cancel.set()
                t.join()
                TTS._cache_writer_cancel.clear()
                print(f"[init_dir] Cache writer cancelled and buffer released")
            else:
                print(f"[init_dir] Waiting for background cache writer to finish...")
                t_wait = time.time()
                t.join()
                print(f"[init_dir] Cache writer finished in {time.time() - t_wait:.2f}s")

        # ── Try cache location 1 first (persistent, survives restarts) ───────
        if os.path.isfile(source_cache_location):
            print(f"[init_dir] Using persistent cache: {source_cache_location} "
                  f"({time.time() - t_start:.2f}s so far)")
            t_init = time.time()
            self.handle = self.init_model(
                source_cache_location,
                audio_encoding=tts_params.audio_encoding_code,
                speaking_rate=tts_params.speaking_rate,
                pitch=tts_params.pitch,
                volume_gain=tts_params.volume_gain,
                sample_rate=tts_params.sample_rate,
                language_code=tts_params.language_code
            )
            print(f"[init_dir] init_model_file took {time.time() - t_init:.2f}s "
                  f"(total {time.time() - t_start:.2f}s)")
            return self.handle

        # ── Try cache location 2 (/tmp/tts) ──────────────────────────────────
        if os.path.isfile(tmp_cache_location):
            print(f"[init_dir] Using /tmp cache: {tmp_cache_location} "
                  f"({time.time() - t_start:.2f}s so far)")
            t_init = time.time()
            self.handle = self.init_model(
                tmp_cache_location,
                audio_encoding=tts_params.audio_encoding_code,
                speaking_rate=tts_params.speaking_rate,
                pitch=tts_params.pitch,
                volume_gain=tts_params.volume_gain,
                sample_rate=tts_params.sample_rate,
                language_code=tts_params.language_code
            )
            print(f"[init_dir] init_model_file took {time.time() - t_init:.2f}s "
                  f"(total {time.time() - t_start:.2f}s)")
            return self.handle

        # ── Cache miss: generate the packed model buffer ──────────────────────
        print(f"[init_dir] No cached .qnn found — running generate_model() "
              f"(this is slow, ~30-60s on first run)")

        # The cancel+join above ensures the bg writer has exited and released
        # its reference to the previous model_buffer. Force a GC now so that
        # memory is actually reclaimed before we allocate the new buffer.
        import gc
        gc.collect()
        print(f"[init_dir] GC collected before generate_model()")

        t_gen = time.time()

        is_model_quantized = 1 if runtimes.get("is_model_quantized") else 0
        model_version_major = 2 if is_model_quantized else 1
        model_version_minor = 0

        print(f"[init_dir] Detected model type: {model_type}")

        # Validate model type is registered
        if not is_model_registered(model_type):
            raise ValueError(
                f"Unknown model type: {model_type}. "
                f"Available types: {list_registered_models()}"
            )
        
        # Get model configuration
        model_config = get_model_config(model_type)
        print(f"[init_dir] Using {model_config.name} model generator")
        
        # Prepare common model generation parameters
        model_gen_params = {
            "bert_model": models_dict.get("model") or models_dict.get("bert_model"),
            "bert_tokenizer": models_dict.get("tokenizer") or models_dict.get("bert_tokenizer"),
            "bert_normalizer": models_dict.get("normalizer") or models_dict.get("bert_normalizer"),
            "g2p_enc_model": models_dict.get("g2p_encoder"),
            "g2p_dec_model": models_dict.get("g2p_decoder"),
            "model_version_major": model_version_major,
            "model_version_minor": model_version_minor,
            "qnn_version_major": int(runtimes.get("qnn_version", {}).get("major")),
            "qnn_version_minor": int(runtimes.get("qnn_version", {}).get("minor")),
            "qnn_version_patch": int(runtimes.get("qnn_version", {}).get("patch")),
            "arch_bit": int(runtimes.get("arch_bit")),
            "is_model_quantized": is_model_quantized,
            "model_lang": runtimes.get("language"),
            "scratch_mem_size_req": int(runtimes.get("scratch_mem_size_req"))
        }
        
        # Add model-specific parameters based on model type
        if model_type == "piper":
            model_gen_params.update({
                "piper_encoder_model": models_dict.get("encoder"),
                "piper_sdp_model": models_dict.get("sdp"),
                "piper_flow_model": models_dict.get("flow"),
                "piper_decoder_model": models_dict.get("decoder"),
            })
        else: # Melo model
            model_gen_params.update({
                "melo_encoder_model": models_dict.get("encoder") or models_dict.get("melo_encoder"),
                "melo_flow_model": models_dict.get("flow") or models_dict.get("melo_flow"),
                "melo_decoder_model": models_dict.get("decoder") or models_dict.get("melo_decoder"),
                "melo_sdp_model": models_dict.get("sdp") or models_dict.get("sdp_model"),
            })

        # Generate model buffer using the registered generator function
        model_buffer = model_config.generate_model_func(**model_gen_params)
        print(f"[init_dir] generate_model() took {time.time() - t_gen:.2f}s")

        # ── Initialize immediately from the in-memory buffer ──────────────────
        # This happens on the calling thread so the handle is ready before we
        # return.  Cache file writes are dispatched to a background daemon
        # thread so they never add latency to the caller.
        t_init = time.time()
        self.handle = self.c_lib.init_model_buffer(
            model_buffer,
            len(model_buffer),
            tts_params
        )
        print(f"[init_dir] init_model_buffer took {time.time() - t_init:.2f}s "
              f"(total so far: {time.time() - t_start:.2f}s)")

        # ── Kick off background cache write ───────────────────────────────────
        cache_thread = threading.Thread(
            target=TTS._write_cache_files_bg,
            args=(
                model_buffer,
                source_cache_location,
                tmp_cache_location,
                store_location_dir,
                TTS._cache_writer_cancel,
                model_type,
            ),
            daemon=True,
            name="tts-cache-writer",
        )
        # Drop the local reference — only the bg thread holds it now.
        # This allows GC to reclaim it as soon as the thread finishes.
        del model_buffer
        TTS._cache_writer_thread = cache_thread
        TTS._cache_writer_path = tmp_cache_location
        cache_thread.start()
        print(f"[init_dir] Cache write dispatched to background thread "
              f"(total so far: {time.time() - t_start:.2f}s)")

        print(f"TTS initialized with model buffer using handle: {self.handle}")
        return self.handle

    def init_model(self, model_location, audio_encoding=0, speaking_rate=1.0, 
             pitch=0.0, volume_gain=0.0, sample_rate=44100, language_code=0):
        """
        Initialize the TTS engine.
        
        Args:
            model_location: Path to the model file (bytes or str)
            audio_encoding: Audio encoding format (default: 0)
            speaking_rate: Speaking rate multiplier (default: 1.0)
            pitch: Pitch adjustment (default: 0.0)
            volume_gain: Volume gain adjustment (default: 0.0)
            sample_rate: Sample rate in Hz (default: 44100)
            language_code: Language code (default: 0 = English)
            
        Returns:
            Handle to the TTS instance
        """
        if isinstance(model_location, str):
            model_location = model_location.encode('utf-8')

        tts_params = self.TTSParams(
            audio_encoding_code=audio_encoding,
            speaking_rate=speaking_rate,
            pitch=pitch,
            volume_gain=volume_gain,
            sample_rate=sample_rate,
            language_code=language_code
        )

        self.handle = self.c_lib.init_model_file(
            model_location, 
            tts_params
        )
        
        print(f"TTS initialized with handle: {self.handle}")
        return self.handle

    def process_with_callback(self, text, callback_function=None):
        """
        Process text with a callback function for streaming audio chunks.

        Args:
            text: Text to synthesize (bytes or str)
            callback_function: Optional Python callback function(chunk_bytes, chunk_size)
                              If None, uses the global callback manager

        Returns:
            Process result code, 0 is success
        """
        if self.handle is None:
            raise RuntimeError("TTS not initialized. Call init() first.")
            
        if isinstance(text, str):
            text = text.encode('utf-8')
        
        # If a custom callback is provided, set it in the global manager
        if callback_function is not None:
            _global_tts_callback_manager.set_handler(callback_function)
        
        # Use the global C callback
        c_callback = _global_tts_callback_manager.get_c_callback()
        
        # Process with the global callback
        result = self.c_lib.process(self.handle, text, c_callback, None)
        
        return result

    def process_to_file(self, text, output_file):
        """
        Process text and save to a file.

        Args:
            text: Text to synthesize (bytes or str)
            output_file: Output file path (bytes or str)
            
        Returns:
            Process result code, 0 is success
        """
        if self.handle is None:
            raise RuntimeError("TTS not initialized. Call init() first.")
            
        if isinstance(text, str):
            text = text.encode('utf-8')
        if isinstance(output_file, str):
            output_file = output_file.encode('utf-8')
            
        result = self.c_lib.process(self.handle, text, None, output_file)
        return result

    def deinit(self):
        """
        Deinitialize the TTS engine and free resources.
        
        Returns:
            Deinit result code
        """
        if self.handle is None:
            return 0

        print("Deinitializing TTS engine...")
        result = self.c_lib.deinit(self.handle)
        self.handle = None
        
        # Clear the callback handler for this instance
        _global_tts_callback_manager.clear_handler()
        
        print("TTS engine deinitialized")
        return result

    def create_chunk_callback(self):
        """
        Create a callback function for receiving audio chunks.
        This method is kept for backward compatibility but now uses the global callback manager.
        
        Returns:
            A callback function that can be passed to process_with_callback
        """
        def chunk_callback(pcm_data, pcm_size):
            """Callback that stores chunks in the audio buffer."""
            # Convert pointer to bytes for storage
            chunk = ctypes.string_at(pcm_data, pcm_size)
            self.audio_buffer.append(chunk)
            print(f"Received chunk: {pcm_size} bytes")
        
        return chunk_callback

    def close(self):
        """Alias for deinit() for consistency with other wrappers."""
        return self.deinit()
