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
import errno
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
        c_lib.tts_benchmark.argtypes = [
            ctypes.c_void_p,  # model_data
            ctypes.c_size_t,  # model_size
            ctypes.c_char_p,  # text
            self.TTSParams,   # tts_params
        ]
        c_lib.tts_benchmark.restype = ctypes.c_char_p
        self._define_tts_set_metrics_enabled_ctype(c_lib)
        self._define_tts_get_kpi_metrics_ctype(c_lib)

    def _define_tts_set_metrics_enabled_ctype(self, c_lib):
        """Define C types for the tts_set_metrics_enabled function.

        C signature:
            void tts_set_metrics_enabled(tts_handle_t handle, int enabled)
        """
        c_lib.tts_set_metrics_enabled.argtypes = [
            ctypes.c_uint64,  # handle
            ctypes.c_int,     # enabled (1 = on, 0 = off)
        ]
        c_lib.tts_set_metrics_enabled.restype = None

    def _define_tts_get_kpi_metrics_ctype(self, c_lib):
        """Define C types for the tts_get_kpi_metrics function.

        C signature:
            const char* tts_get_kpi_metrics(tts_handle_t handle)
        """
        c_lib.tts_get_kpi_metrics.argtypes = [ctypes.c_uint64]  # handle
        c_lib.tts_get_kpi_metrics.restype = ctypes.c_char_p

    @staticmethod
    def _validate_model_params(model_params: dict) -> bool:
        """Validate TTS model parameters against the CSIM constraints.

        Args:
            model_params: dict with keys audio_encoding, speaking_rate, pitch,
                          volume_gain, and sample_rate.

        Returns:
            True if all parameters are valid, False otherwise.
        """
        VALID_AUDIO_ENCODINGS = {0, 1, 2, 3, 4}  # LINEAR16, MP3, OGG_OPUS, MULAW, ALAW
        if model_params["audio_encoding"] not in VALID_AUDIO_ENCODINGS:
            print(
                f"Invalid audio_encoding {model_params['audio_encoding']}. "
                f"Must be one of {VALID_AUDIO_ENCODINGS} (LINEAR16=0, MP3=1, OGG_OPUS=2, MULAW=3, ALAW=4)"
            )
            return False
        if not (0.25 <= model_params["speaking_rate"] <= 4.0):
            print(
                f"Invalid speaking_rate {model_params['speaking_rate']}. "
                f"Must be between 0.25 and 4.0"
            )
            return False
        if not (-20.0 <= model_params["pitch"] <= 20.0):
            print(
                f"Invalid pitch {model_params['pitch']}. "
                f"Must be between -20.0 and 20.0"
            )
            return False
        if not (-96.0 <= model_params["volume_gain"] <= 16.0):
            print(
                f"Invalid volume_gain {model_params['volume_gain']}. "
                f"Must be between -96.0 and 16.0 dB"
            )
            return False
        return True

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
    def _write_cache_files_bg(model_buffer, tmp_cache_location, store_location_dir,
                              cancel_event: threading.Event, model_type,
                              model_base_name: str = ""):
        """Write packed .qnn cache file to TTS_MODEL_STORE_DIR in a background thread."""
        try:
            model_config = get_model_config(model_type)
            generate_packed_model_file = model_config.generate_packed_file_func
        except ValueError as e:
            print(f"[cache_writer] Error: {e}")
            return

        def _remove_stale(cache_dir: str, current_filename: str):
            if not model_base_name:
                return
            try:
                import fnmatch
                pattern = f"{model_base_name}_v*.qnn"
                for fname in os.listdir(cache_dir):
                    if fnmatch.fnmatch(fname, pattern) and fname != current_filename:
                        stale = os.path.join(cache_dir, fname)
                        try:
                            os.remove(stale)
                            print(f"[cache_writer] Removed stale cache: {stale}")
                        except Exception as e:
                            print(f"[cache_writer] Could not remove stale {stale}: {e}")
            except FileNotFoundError:
                pass
            except Exception as e:
                print(f"[cache_writer] Error scanning for stale caches: {e}")

        if cancel_event.is_set():
            print(f"[cache_writer] Cancelled before cache write")
            return
        # If the cache file already exists, skip the write — it's already good.
        if os.path.exists(tmp_cache_location):
            print(f"[cache_writer] Cache already exists, skipping write: {tmp_cache_location}")
            return
        tmp_write_path = tmp_cache_location + ".tmp"
        try:
            if not os.path.isdir(store_location_dir):
                os.makedirs(store_location_dir)
            tmp_filename = os.path.basename(tmp_cache_location)
            _remove_stale(store_location_dir, tmp_filename)
            # Write to a .tmp file first — never touch the existing good cache
            # until the new write succeeds.
            if os.path.exists(tmp_write_path):
                os.remove(tmp_write_path)
            t = time.time()
            generate_packed_model_file(tmp_write_path, model_buffer)
            os.replace(tmp_write_path, tmp_cache_location)
            print(f"[cache_writer] Wrote cache to {tmp_cache_location} "
                  f"in {time.time() - t:.2f}s")
        except OSError as e:
            if e.errno == errno.ENOSPC:
                print(f"[cache_writer] No space left on device — attempting LRU eviction")
                if wu.evict_lru_cache(store_location_dir, len(model_buffer) if model_buffer else 0,
                                      exclude_path=tmp_cache_location):
                    try:
                        generate_packed_model_file(tmp_write_path, model_buffer)
                        os.replace(tmp_write_path, tmp_cache_location)
                        print(f"[cache_writer] Wrote cache after eviction: {tmp_cache_location}")
                    except Exception as retry_err:
                        print(f"[cache_writer] Retry after eviction failed: {retry_err}")
                        if os.path.exists(tmp_write_path):
                            try:
                                os.remove(tmp_write_path)
                            except Exception:
                                pass
                else:
                    print(f"[cache_writer] Eviction failed — skipping cache write for {tmp_cache_location}")
            else:
                print(f"[cache_writer] Could not write cache to {tmp_cache_location}: {e}")
            if os.path.exists(tmp_write_path):
                try:
                    os.remove(tmp_write_path)
                except Exception:
                    pass
        finally:
            model_buffer = None

    def init_dir(self, model_dir_location, tts_param):
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
            
            voices = config_json["voices"]
            model_params = {
                "audio_encoding": tts_param.get("audio_encoding") or voices[0]["audio_encoding"],
                "speaking_rate": tts_param.get("speaking_rate") or voices[0]["speaking_rate"],
                "pitch": tts_param.get("pitch") or voices[0]["pitch"],
                "volume_gain": tts_param.get("volume_gain") or voices[0]["volume_gain"],
                "sample_rate": voices[0]["sample_rate"], # Use voice default sample rate unless CSIM changes to accomodate sample rate changes
            }
            
            # Validate model_params against tts_config_t constraints
            if not self._validate_model_params(model_params):
                return

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
                            audio_encoding=model_params["audio_encoding"],
                            speaking_rate=model_params["speaking_rate"],
                            pitch=model_params["pitch"],
                            volume_gain=model_params["volume_gain"],
                            sample_rate=model_params["sample_rate"],
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

        print(model_params)

        tts_params = self.TTSParams(
            audio_encoding=model_params["audio_encoding"],
            speaking_rate=model_params["speaking_rate"],
            pitch=model_params["pitch"],
            volume_gain=model_params["volume_gain"],
            sample_rate=model_params["sample_rate"],
            language_code=voices[0]["language_code"]
        )

        runtimes = config_json["runtime"]

        qnn_ver = runtimes.get("qnn_version", {})
        model_base_name = config_json['name']
        model_name = (
            f"{model_base_name}"
            f"_v{qnn_ver.get('major', 0)}"
            f".{qnn_ver.get('minor', 0)}"
            f".{qnn_ver.get('patch', 0)}.qnn"
        )

        # ── Single cache location: TTS_MODEL_STORE_DIR (/tmp/audio-cache) ─────
        store_location_dir = TTS_MODEL_STORE_DIR
        tmp_cache_location = os.path.join(store_location_dir, model_name)

        # ── Cancel or wait for any in-progress cache write ────────────────────
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

        # ── Try cache (/tmp/audio-cache) ──────────────────────────────────────
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
            self._resolved_qnn_path = tmp_cache_location
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
                tmp_cache_location,
                store_location_dir,
                TTS._cache_writer_cancel,
                model_type,
                model_base_name,
            ),
            daemon=True,
            name="tts-cache-writer",
        )
        # Drop the local reference — only the bg thread holds it now.
        # This allows GC to reclaim it as soon as the thread finishes.
        del model_buffer
        TTS._cache_writer_thread = cache_thread
        TTS._cache_writer_path = tmp_cache_location
        self._resolved_qnn_path = tmp_cache_location
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

    def _parse_benchmark_metrics(self, metrics_str: str) -> dict:
        """Parse a TTS benchmark metrics string into a dict.

        Input: "init=622ms, total_latency=1030ms, latency_first=350ms, latency_subsequent=75ms"
        Output: {"init": 622, "total_latency": 1030, "latency_first": 350, "latency_subsequent": 75}
        """
        result = {}
        if not metrics_str:
            return result
        for part in metrics_str.split(','):
            part = part.strip()
            if '=' not in part:
                continue
            key, val = part.split('=', 1)
            val = val.strip().rstrip('ms').strip()
            try:
                result[key.strip()] = int(val)
            except ValueError:
                result[key.strip()] = val
        return result

    def benchmark(self, text: str, model_dir: str, tts_param: dict) -> dict:
        """Run a full TTS benchmark (init → synthesize → deinit) and return metrics.

        Resolves the model buffer from cache or generates it from assets, then
        passes the bytes directly to the C tts_benchmark() function which manages
        its own TTS lifecycle internally.

        Args:
            text:      Text to synthesize during the benchmark.
            model_dir: Path to the model directory.
            tts_param: Dict with optional TTS parameter overrides.

        Returns:
            dict with keys: init, total_latency, latency_first, latency_subsequent,
            model_size (all values in milliseconds as ints, model_size in bytes).
            Empty dict on failure.
        """
        if isinstance(text, str):
            text = text.encode('utf-8')

        model_dir_str = model_dir if isinstance(model_dir, str) else model_dir.decode('utf-8')

        config_path = wu.search_file("config.json", model_dir_str)
        config_json = wu.open_file(config_path)
        voices = config_json["voices"]
        model_params = {
            "audio_encoding": tts_param.get("audio_encoding") or voices[0]["audio_encoding"],
            "speaking_rate":  tts_param.get("speaking_rate")  or voices[0]["speaking_rate"],
            "pitch":          tts_param.get("pitch")          or voices[0]["pitch"],
            "volume_gain":    tts_param.get("volume_gain")    or voices[0]["volume_gain"],
            "sample_rate":    voices[0]["sample_rate"],
        }
        tts_params = self.TTSParams(
            audio_encoding_code=model_params["audio_encoding"],
            speaking_rate=model_params["speaking_rate"],
            pitch=model_params["pitch"],
            volume_gain=model_params["volume_gain"],
            sample_rate=model_params["sample_rate"],
            language_code=voices[0]["language_code"],
        )

        runtimes = config_json["runtime"]
        qnn_ver = runtimes.get("qnn_version", {})
        model_base_name = config_json['name']
        model_name = (
            f"{model_base_name}"
            f"_v{qnn_ver.get('major', 0)}"
            f".{qnn_ver.get('minor', 0)}"
            f".{qnn_ver.get('patch', 0)}.qnn"
        )
        tmp_cache_location = os.path.join(TTS_MODEL_STORE_DIR, model_name)

        # ── Resolve model buffer ──────────────────────────────────────────────
        # Wait for any in-progress background cache write for this model
        t = TTS._cache_writer_thread
        if t is not None and t.is_alive() and TTS._cache_writer_path == tmp_cache_location:
            print(f"[benchmark] Waiting for background cache writer to finish...")
            t_wait = time.time()
            t.join()
            print(f"[benchmark] Cache writer finished in {time.time() - t_wait:.2f}s")

        model_buffer = None
        if os.path.isfile(tmp_cache_location):
            print(f"[benchmark] Loading model from cache: {tmp_cache_location}")
            with open(tmp_cache_location, 'rb') as f:
                model_buffer = f.read()
        else:
            print(f"[benchmark] Cache miss — generating model from assets")
            models_dict = wu.match_files_to_assets(config_json, model_dir_str)
            model_type = config_json.get("model_type", "melo").lower()
            if not is_model_registered(model_type):
                raise ValueError(f"Unknown model type: {model_type}")
            model_config = get_model_config(model_type)

            is_model_quantized = 1 if runtimes.get("is_model_quantized") else 0
            model_gen_params = {
                "bert_model":        models_dict.get("model") or models_dict.get("bert_model"),
                "bert_tokenizer":    models_dict.get("tokenizer") or models_dict.get("bert_tokenizer"),
                "bert_normalizer":   models_dict.get("normalizer") or models_dict.get("bert_normalizer"),
                "g2p_enc_model":     models_dict.get("g2p_encoder"),
                "g2p_dec_model":     models_dict.get("g2p_decoder"),
                "model_version_major": 2 if is_model_quantized else 1,
                "model_version_minor": 0,
                "qnn_version_major": int(qnn_ver.get("major")),
                "qnn_version_minor": int(qnn_ver.get("minor")),
                "qnn_version_patch": int(qnn_ver.get("patch")),
                "arch_bit":          int(runtimes.get("arch_bit")),
                "is_model_quantized": is_model_quantized,
                "model_lang":        runtimes.get("language"),
                "scratch_mem_size_req": int(runtimes.get("scratch_mem_size_req"))
            }
            if model_type == "piper":
                model_gen_params.update({
                    "piper_encoder_model": models_dict.get("encoder"),
                    "piper_sdp_model":     models_dict.get("sdp"),
                    "piper_flow_model":    models_dict.get("flow"),
                    "piper_decoder_model": models_dict.get("decoder"),
                })
            else:
                model_gen_params.update({
                    "melo_encoder_model": models_dict.get("encoder") or models_dict.get("melo_encoder"),
                    "melo_flow_model":    models_dict.get("flow") or models_dict.get("melo_flow"),
                    "melo_decoder_model": models_dict.get("decoder") or models_dict.get("melo_decoder"),
                    "melo_sdp_model":     models_dict.get("sdp") or models_dict.get("sdp_model"),
                })
            model_buffer = model_config.generate_model_func(**model_gen_params)

            # Kick off background cache write for next run
            store_location_dir = TTS_MODEL_STORE_DIR
            cache_thread = threading.Thread(
                target=TTS._write_cache_files_bg,
                args=(model_buffer, tmp_cache_location, store_location_dir,
                      TTS._cache_writer_cancel, model_type, model_base_name),
                daemon=True,
                name="tts-cache-writer",
            )
            TTS._cache_writer_thread = cache_thread
            TTS._cache_writer_path = tmp_cache_location
            cache_thread.start()

        if not model_buffer:
            return {}

        # ── Call C benchmark with buffer ──────────────────────────────────────
        model_buf_c = ctypes.create_string_buffer(model_buffer)
        model_ptr = ctypes.cast(model_buf_c, ctypes.c_void_p)
        model_size = len(model_buffer)

        raw = self.c_lib.tts_benchmark(model_ptr, ctypes.c_size_t(model_size), text, tts_params)
        if raw is None:
            return {}
        metrics_str = raw.decode('utf-8') if isinstance(raw, bytes) else raw
        result = self._parse_benchmark_metrics(metrics_str)
        result["model_size"] = model_size
        return result

    def set_metrics_enabled(self, enabled: bool) -> None:
        """Enable or disable KPI metrics collection for this TTS instance.

        When enabled, each call to process_with_callback() or process_to_file()
        records timing metrics that can be retrieved with get_kpi_metrics().
        Metrics collection is disabled by default.

        Args:
            enabled: True to enable metrics collection, False to disable.

        Raises:
            RuntimeError: If the TTS instance has not been initialized.
        """
        if self.handle is None:
            raise RuntimeError("TTS not initialized. Call init_model() or init_dir() first.")

        self.c_lib.tts_set_metrics_enabled(self.handle, int(enabled))
        print(f"TTS metrics collection {'enabled' if enabled else 'disabled'}")

    def get_kpi_metrics(self) -> dict:
        """Return KPI metrics from the most recent process() call as a dict.

        Metrics are only populated when metrics collection has been enabled via
        set_metrics_enabled(True) before calling process_with_callback() or
        process_to_file().

        Returns:
            dict with integer millisecond values for the keys:
                - ``init``               — time spent in init_model_buffer()
                - ``total_latency``      — end-to-end time for the process() call
                - ``latency_first``      — time to receive the first audio chunk
                - ``latency_subsequent`` — time between subsequent audio chunks

            Returns an empty dict if the handle is invalid, metrics were never
            collected, or the metrics string could not be parsed.

        Example::

            tts.set_metrics_enabled(True)
            tts.process_with_callback(b"Hello world", callback)
            metrics = tts.get_kpi_metrics()
            # {'init': 622, 'total_latency': 1030, 'latency_first': 350,
            #  'latency_subsequent': 75}

        Raises:
            RuntimeError: If the TTS instance has not been initialized.
        """
        if self.handle is None:
            raise RuntimeError("TTS not initialized. Call init_model() or init_dir() first.")

        raw = self.c_lib.tts_get_kpi_metrics(self.handle)
        if raw is None:
            return {}

        metrics_str = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        return self._parse_benchmark_metrics(metrics_str)

    def close(self):
        """Alias for deinit() for consistency with other wrappers."""
        return self.deinit()
