# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Translation Wrapper with Global Callback Management

This version uses a global callback manager to ensure:
1. C function pointers remain valid (no dangling pointers)
2. Callbacks can be dynamically updated without re-registering with C
3. Multiple wrapper instances can coexist with different callbacks
"""

import ctypes
import os
import sys
import time
import traceback
import threading
from concurrent.futures import ThreadPoolExecutor
import wrapper_utils as wu
from translation_buffer_create import generate_model_blob_from_bytes

T2T_MODEL_STORE_DIR = os.environ.get('T2T_MODEL_STORE_DIR', '/tmp/audio-cache')
# ----------------------------------------------------------------------
# Callback type definitions (match the C typedefs)
# ----------------------------------------------------------------------
# typedef void (*OnResultFn)(void* user_data, const char* result);
OnResultFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p)

# typedef void (*OnDoneFn)(void* user_data);
OnDoneFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p)

# typedef void (*OnErrorFn)(void* user_data, int32_t errorCode);
OnErrorFn = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int32)

# ----------------------------------------------------------------------
# Global Callback Manager
# ----------------------------------------------------------------------
class TranslationCallbackManager:
    """Manages global callback routing with strong references.
    
    This ensures that:
    1. C function pointers remain valid (no dangling pointers)
    2. Callbacks can be dynamically updated without re-registering with C
    3. Multiple wrapper instances can coexist with different callbacks
    """
    
    def __init__(self):
        # Strong references to C callback functions (never garbage collected)
        self._c_on_result = None
        self._c_on_done = None
        self._c_on_error = None
        
        # Current Python callback handlers (can be updated)
        self._current_result_handler = None
        self._current_done_handler = None
        self._current_error_handler = None
        
        # Lock for thread-safe callback updates
        self._lock = threading.Lock()
        
        # Initialize the global C callbacks once
        self._initialize_global_callbacks()
    
    def _initialize_global_callbacks(self):
        """Create global C callback functions that route to current handlers."""
        
        # Import PyGILState functions for thread safety
        import ctypes
        pythonapi = ctypes.pythonapi
        PyGILState_Ensure = pythonapi.PyGILState_Ensure
        PyGILState_Ensure.restype = ctypes.c_int
        PyGILState_Release = pythonapi.PyGILState_Release
        PyGILState_Release.argtypes = [ctypes.c_int]
        
        @OnResultFn
        def _global_on_result(user_data, result_ptr):
            """Global C callback that routes to the current result handler."""
            # Acquire GIL since this might be called from a C++ thread
            gil_state = PyGILState_Ensure()
            try:
                with self._lock:
                    handler = self._current_result_handler
                
                if handler:
                    try:
                        if result_ptr:
                            # Try to safely decode the result
                            try:
                                result_bytes = ctypes.cast(result_ptr, ctypes.c_char_p).value
                                if result_bytes is None:
                                    print("Warning: result_bytes is None in global callback")
                                    return
                                result = result_bytes.decode('utf-8', errors='replace')
                                handler(result)
                            except Exception as decode_error:
                                print(f"Error decoding result in callback: {decode_error}")
                                traceback.print_exc()
                                # Try to pass the raw bytes if decoding fails
                                try:
                                    handler(str(result_bytes))
                                except:
                                    pass
                        else:
                            print("Warning: result_ptr is NULL in global callback")
                    except Exception as e:
                        print(f"Error in global result callback handler: {e}")
                        traceback.print_exc()
            except Exception as e:
                print(f"Critical error in global result callback: {e}")
                traceback.print_exc()
            finally:
                # Always release GIL
                PyGILState_Release(gil_state)
        
        @OnDoneFn
        def _global_on_done(user_data):
            """Global C callback that routes to the current done handler."""
            # Acquire GIL since this might be called from a C++ thread
            gil_state = PyGILState_Ensure()
            try:
                with self._lock:
                    handler = self._current_done_handler
                
                if handler:
                    try:
                        handler()
                    except Exception as e:
                        print(f"Error in global done callback handler: {e}")
                        traceback.print_exc()
            except Exception as e:
                print(f"Critical error in global done callback: {e}")
                traceback.print_exc()
            finally:
                # Always release GIL
                PyGILState_Release(gil_state)
        
        @OnErrorFn
        def _global_on_error(user_data, error_code):
            """Global C callback that routes to the current error handler."""
            # Acquire GIL since this might be called from a C++ thread
            gil_state = PyGILState_Ensure()
            try:
                with self._lock:
                    handler = self._current_error_handler
                
                if handler:
                    try:
                        handler(error_code)
                    except Exception as e:
                        print(f"Error in global error callback handler: {e}")
                        traceback.print_exc()
            except Exception as e:
                print(f"Critical error in global error callback: {e}")
                traceback.print_exc()
            finally:
                # Always release GIL
                PyGILState_Release(gil_state)
        
        # Store strong references to prevent garbage collection
        self._c_on_result = _global_on_result
        self._c_on_done = _global_on_done
        self._c_on_error = _global_on_error
        
        print("Global translation callbacks initialized")
    
    def set_handlers(self, on_result=None, on_done=None, on_error=None):
        """Update the current callback handlers.
        
        Args:
            on_result: Python function to handle results
            on_done: Python function to handle completion
            on_error: Python function to handle errors
        """
        with self._lock:
            if on_result is not None:
                self._current_result_handler = on_result
            if on_done is not None:
                self._current_done_handler = on_done
            if on_error is not None:
                self._current_error_handler = on_error
        
        print(f"Updated translation callback handlers: result={on_result is not None}, done={on_done is not None}, error={on_error is not None}")
    
    def clear_handlers(self):
        """Clear all current handlers (but keep C callbacks alive)."""
        with self._lock:
            self._current_result_handler = None
            self._current_done_handler = None
            self._current_error_handler = None
        
        print("Cleared translation callback handlers")
    
    def get_c_callbacks(self):
        """Get the global C callback function pointers.
        
        Returns:
            Tuple of (on_result, on_done, on_error) C function pointers
        """
        return (self._c_on_result, self._c_on_done, self._c_on_error)

# Global singleton instance
_global_translation_callback_manager = TranslationCallbackManager()


class TranslationWrapper:
    """Encapsulates the translation engine lifecycle with global callback management.

    Example:
        wrapper = TranslationWrapper(
            model_path=b"/path/to/model.qnn",
            input_lang=b"Chinese",
            output_lang=b"English"
        )
        wrapper.process("示例文本")
        wrapper.close()
    """
    
    # Class-level variables (shared across all instances)
    _shared_wrapper = None
    _library_loaded = False
    _shared_cb_handle = None  # Global callback handle, never destroyed
    _callback_initialized = False
    _instance_count = 0  # Track number of active instances
    _current_engine_model = None  # Track what model is currently loaded in the C++ engine
    _current_engine_input_lang = None
    _current_engine_output_lang = None

    def __init__(self, model_path: bytes, model_dir:str, input_lang: bytes, output_lang: bytes, lib_path: str = None):
        # Increment instance count
        TranslationWrapper._instance_count += 1
        self.instance_id = TranslationWrapper._instance_count
        print(f"[Instance {self.instance_id}] Initializing TranslationWrapper...")
        
        # Load the shared library only once (class-level)
        if not TranslationWrapper._library_loaded:
            TranslationWrapper._shared_wrapper = self._load_library(lib_path)
            self._setup_c_api(TranslationWrapper._shared_wrapper)
            TranslationWrapper._library_loaded = True
            print(f"[Instance {self.instance_id}] Translation library loaded for the first time")
        else:
            print(f"[Instance {self.instance_id}] Reusing already loaded translation library")
        
        # Use the shared library instance
        self._wrapper = TranslationWrapper._shared_wrapper
        
        # Store model parameters for potential reinitialization
        self.current_model_path = model_path
        self.current_input_lang = input_lang
        self.current_output_lang = output_lang
        
        # Storage for translation results
        self.final_text = []
        
        # Set up instance-specific callback handlers
        def _on_result(result):
            """Instance-specific result handler."""
            try:
                print(f"[Instance {self.instance_id}] _on_result callback called")
                print(f"[Instance {self.instance_id}] [Result] {result}")
                self.final_text.append(result)
            except Exception as e:
                print(f"[Instance {self.instance_id}] Error in _on_result: {e}")
                traceback.print_exc()
        
        def _on_done():
            """Instance-specific done handler."""
            print(f"[Instance {self.instance_id}] [Done] Translation finished callback called.")
        
        def _on_error(error_code):
            """Instance-specific error handler."""
            print(f"[Instance {self.instance_id}] [Error] Callback called with code {error_code}")
        
        # Store instance handlers for later use
        self._instance_on_result = _on_result
        self._instance_on_done = _on_done
        self._instance_on_error = _on_error

        # Always initialise from the model directory — init_dir() resolves
        # whether to use the pre-built model_file or build from assets.
        self.init_dir(model_dir, input_lang, output_lang)
    
    def get_language_code(self, language_name: bytes) -> str:
        """Convert language name to language code for file naming."""
        # Decode bytes to string and normalize
        lang_str = language_name.decode('utf-8').lower()

        # Language name to code mapping
        lang_map = {
            'english': 'en',
            'chinese': 'zh',
            'spanish': 'es',
            'french': 'fr',
            'german': 'de',
            'japanese': 'ja',
            'korean': 'ko',
            'arabic': 'ar',
            'russian': 'ru',
            'portuguese': 'pt',
            'italian': 'it'
        }

        return lang_map.get(lang_str, lang_str)

    def init_dir(self, model_dir: str, input_lang: bytes, output_lang: bytes):
        """
        Initialise the translation engine from a model directory.

        Resolution order:
          1. config.json has a ``model_file`` entry AND the file exists → fast path.
          2. /tmp/translation/ has a cached .qnn file → fast path.
          3. Otherwise build from individual asset files → slow path.
        """
        if not os.path.isdir(model_dir):
            print(f"[Instance {self.instance_id}] init_dir: '{model_dir}' is not a valid directory")
            return

        print(f"[Instance {self.instance_id}] init_dir: {model_dir}")

        config_file = "config.json"
        if not wu.check_config(config_file, model_dir):
            print(f"[Instance {self.instance_id}] init_dir: config.json not found in {model_dir}")
            return
        config_json = wu.get_config(config_file, model_dir)

        input_code  = self.get_language_code(input_lang)
        output_code = self.get_language_code(output_lang)

        # Fast path 1: model_file in config.json
        if "model_file" in config_json:
            model_file_path = os.path.join(model_dir, config_json["model_file"])
            if os.path.isfile(model_file_path):
                print(f"[Instance {self.instance_id}] init_dir: using model_file from config: {model_file_path}")
                self._initialize_engine_and_callbacks(model_file_path.encode("utf-8"), input_lang, output_lang)
                return
            else:
                print(f"[Instance {self.instance_id}] init_dir: model_file not found, continuing")

        # Fast path 2: cached /tmp file
        t2t_temp_file = f"{T2T_MODEL_STORE_DIR}/translation_{input_code}_{output_code}.qnn"
        if os.path.isfile(t2t_temp_file):
            print(f"[Instance {self.instance_id}] init_dir: using cached temp file: {t2t_temp_file}")
            self._initialize_engine_and_callbacks(t2t_temp_file.encode("utf-8"), input_lang, output_lang)
            return

        # Slow path: build from assets
        print(f"[Instance {self.instance_id}] init_dir: building from assets")
        self._initialize_engine_and_callbacks_with_dir(model_dir, input_lang, output_lang)

    def _setup_engine_and_callback(self) -> object:
        """Create/reuse the global callback handle, create a fresh engine, register callback.

        Returns:
            cpp_cb: the C++ callback pointer to pass to _finish_init.
        """
        # 1. Create the global callback handle if it doesn't exist (class-level, shared)
        if not TranslationWrapper._callback_initialized:
            print(f"[Instance {self.instance_id}] Creating global translation callback handle...")
            c_on_result, c_on_done, c_on_error = _global_translation_callback_manager.get_c_callbacks()
            TranslationWrapper._shared_cb_handle = self._wrapper.create_translation_callback(
                c_on_result,
                c_on_done,
                c_on_error,
                None  # user_data can be NULL or a pointer to custom data
            )
            if not TranslationWrapper._shared_cb_handle:
                raise RuntimeError("Failed to create global translation callback")
            TranslationWrapper._callback_initialized = True
            print(f"[Instance {self.instance_id}] Global callback handle created: {TranslationWrapper._shared_cb_handle}")
        else:
            print(f"[Instance {self.instance_id}] Reusing global callback handle: {TranslationWrapper._shared_cb_handle}")

        # 2. Create a fresh translation engine for this instance
        print(f"[Instance {self.instance_id}] Creating translation engine...")
        self._engine = self._wrapper.create_translation_engine()
        if not self._engine:
            raise RuntimeError(f"Failed to create translation engine for instance {self.instance_id}")
        print(f"[Instance {self.instance_id}] Engine handle created: {self._engine}")

        # 3. Get the cpp_cb pointer (with NULL recovery) and register BEFORE init()
        print("Registering global callback with engine BEFORE init()...")
        cpp_cb = self._wrapper.get_translation_result_callback(TranslationWrapper._shared_cb_handle)
        if not cpp_cb:
            print("WARNING: Got NULL C++ callback pointer, trying to recreate callback handle")
            c_on_result, c_on_done, c_on_error = _global_translation_callback_manager.get_c_callbacks()
            TranslationWrapper._shared_cb_handle = self._wrapper.create_translation_callback(
                c_on_result,
                c_on_done,
                c_on_error,
                None
            )
            if not TranslationWrapper._shared_cb_handle:
                raise RuntimeError("Failed to recreate global translation callback")
            print(f"Recreated global callback handle: {TranslationWrapper._shared_cb_handle}")
            cpp_cb = self._wrapper.get_translation_result_callback(TranslationWrapper._shared_cb_handle)
            if not cpp_cb:
                raise RuntimeError("Still got NULL C++ callback pointer after recreation")

        print(f"Got C++ callback pointer: {cpp_cb}")
        self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
        print("Callback registered successfully BEFORE init()")
        print("Callback registration verification complete")
        return cpp_cb

    def _finish_init(self, init_ret, retry_fn, cpp_cb):
        """Handle init error recovery, re-register callback, and activate handlers.

        Args:
            init_ret: return code from the first init call.
            retry_fn: zero-argument callable that retries the init and returns the new code.
            cpp_cb:   the C++ callback pointer returned by _setup_engine_and_callback.
        """
        if init_ret != 0:
            error_msg = f"Engine initialization failed with code {init_ret}"
            print(f"ERROR: {error_msg}")
            print("The C++ wrapper already tried multiple recovery attempts")
            print("This indicates a persistent DSP/RPC resource conflict")
            print("Attempting Python-level recovery with longer delay...")
            try:
                print("Waiting 5 seconds for complete DSP resource release...")
                time.sleep(5.0)
                print("Final recovery attempt...")
                final_ret = retry_fn()
                print(f"Final recovery init returned: {final_ret}")
                if final_ret == 0:
                    print("Python-level recovery successful!")
                else:
                    print(f"Python-level recovery also failed with code {final_ret}")
                    raise RuntimeError(f"All recovery attempts failed. Final error code: {final_ret}")
            except Exception as recovery_error:
                print(f"Recovery attempt failed: {recovery_error}")
                raise RuntimeError(error_msg)
        else:
            print("Engine initialized successfully")

        # Re-register callback AFTER init() (init may reset internal state)
        print("Re-registering callback AFTER init() to ensure it's set...")
        self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
        print("Callback re-registered successfully AFTER init()")

        # Activate this instance's handlers
        _global_translation_callback_manager.set_handlers(
            on_result=self._instance_on_result,
            on_done=self._instance_on_done,
            on_error=self._instance_on_error
        )
        print(f"[Instance {self.instance_id}] Set as active callback handler")

    def _initialize_engine_and_callbacks_with_dir(self, model_dir: bytes, input_lang: bytes, output_lang: bytes):
        """Initialize or reinitialize the translation engine and callbacks."""

        print(f"[Instance {self.instance_id}] _initialize_engine_and_callbacks")

        cpp_cb = self._setup_engine_and_callback()

        # 4. Initialize the engine with the model and languages AFTER callback registration
        config_file = "config.json"
        if not wu.check_config(config_file, model_dir):
            return

        config_json = wu.get_config(config_file, model_dir)

        input_code = self.get_language_code(input_lang)
        output_code = self.get_language_code(output_lang)
        model_files = {
            "encoder_path": config_json["assets"].get("encoder_path"),
            "decoder_path": config_json["assets"].get("decoder_path"),
            "tokenizer_path": config_json["assets"].get("tokenizer_path"),
            "lookups_path": config_json["assets"].get("lookups_path")
        }

        models_dict = wu.check_assests(config_json, model_files, model_dir)

        if not models_dict:
            return
        if not wu.check_t2t_runtime(config_json, input_lang_code=input_code):
            return
        runtimes = config_json["runtime"]

        print(f"[Instance {self.instance_id}] Loading model assets:")
        for key, path in models_dict.items():
            size = os.path.getsize(path) if os.path.isfile(path) else -1
            print(f"[Instance {self.instance_id}]   {key}: {path} ({size} bytes)")

        def _read(path):
            with open(path, 'rb') as f:
                return f.read()

        with ThreadPoolExecutor(max_workers=4) as pool:
            f_encoder   = pool.submit(_read, models_dict["encoder_path"])
            f_decoder   = pool.submit(_read, models_dict["decoder_path"])
            f_tokenizer = pool.submit(_read, models_dict["tokenizer_path"])
            f_lookups   = pool.submit(_read, models_dict["lookups_path"])
            encoder, decoder, tokenizer, tokenizer_auto_gen = (
                f_encoder.result(), f_decoder.result(),
                f_tokenizer.result(), f_lookups.result()
            )

        model_buffer = generate_model_blob_from_bytes(
            opus_encoder_model=encoder,
            opus_decoder_model=decoder,
            tokenizer_autogen=tokenizer,
            tokenizer_autogen_lookups=tokenizer_auto_gen,
            qnn_version_major=runtimes["qnn_version"].get("major"),
            qnn_version_minor=runtimes["qnn_version"].get("minor"),
            qnn_version_patch=runtimes["qnn_version"].get("patch"),
            arch=runtimes.get("arch"),
            enc_model_max_seq_len=runtimes.get("enc_model_max_seq_len"),
            dec_model_max_seq_len=runtimes.get("dec_model_max_seq_len"),
            rep_penalty=runtimes.get("rep_penalty"),
            model_lang=runtimes.get("model_lang"),
            scratch_mem_size_req=runtimes.get("scratch_mem_size_req"),
        )

        # Start cache write immediately — runs concurrently with the 3-minute DSP init below
        try:
            cache_thread = threading.Thread(
                target=self._write_cache_file,
                args=(model_buffer, input_code, output_code),
                daemon=True
            )
            cache_thread.start()
            print("Background cache file creation started")
        except Exception as e:
            print(f"Could not start background cache creation: {e}")

        self._model_buf = ctypes.create_string_buffer(model_buffer)  # owns memory
        model_ptr = ctypes.cast(self._model_buf, ctypes.c_void_p)
        model_size = ctypes.c_size_t(len(model_buffer))
        init_ret = self._wrapper.translation_engine_init_from_buffer(
            self._engine,
            model_ptr,
            model_size,
            input_lang,
            output_lang
        )
        print(f"Engine init returned: {init_ret}")

        self._finish_init(
            init_ret,
            lambda: self._wrapper.translation_engine_init_from_buffer(
                self._engine, model_ptr, model_size, input_lang, output_lang
            ),
            cpp_cb
        )

        # Update current model parameters for this instance
        self.current_input_lang = input_lang
        self.current_output_lang = output_lang

        # Update class-level tracking of what's loaded in the C++ engine
        TranslationWrapper._current_engine_input_lang = input_lang
        TranslationWrapper._current_engine_output_lang = output_lang

    def _write_cache_file(self, model_buffer, input_code, output_code):
        """Write model buffer to the cache directory (synchronous, run in a background thread)."""
        t2t_tmp_dir = T2T_MODEL_STORE_DIR
        try:
            os.makedirs(t2t_tmp_dir, exist_ok=True)

            cache_filename = f"translation_{input_code}_{output_code}.qnn"
            cache_file_path = os.path.join(t2t_tmp_dir, cache_filename)

            # Remove stale file for this language pair before writing
            if os.path.exists(cache_file_path):
                try:
                    os.remove(cache_file_path)
                except Exception as e:
                    print(f"Could not remove old cache file {cache_file_path}: {e}")

            # Write to temporary file first (atomic operation)
            temp_file_path = cache_file_path + ".tmp"

            print(f"Creating cache file: {cache_file_path}")

            with open(temp_file_path, 'wb') as f:
                f.write(model_buffer)

            # Atomic rename - ensures file is complete before it's available
            os.rename(temp_file_path, cache_file_path)
            print(f"Cache file created successfully: {cache_file_path}")

        except Exception as e:
            print(f"Failed to create cache file: {e}")
            # Clean up temp file if it exists
            temp_file_path = os.path.join(t2t_tmp_dir, f"translation_{input_code}_{output_code}.qnn.tmp")
            if os.path.exists(temp_file_path):
                try:
                    os.remove(temp_file_path)
                except:
                    pass

    def _cleanup_cache_dir(self, cache_dir: str):
        """Remove old .qnn cache files from the cache directory."""
        try:
            for filename in os.listdir(cache_dir):
                if filename.endswith('.qnn') or filename.endswith('.qnn.tmp'):
                    file_path = os.path.join(cache_dir, filename)
                    try:
                        os.remove(file_path)
                        print(f"Removed old cache file: {file_path}")
                    except Exception as e:
                        print(f"Could not remove old cache file {file_path}: {e}")

        except Exception as e:
            print(f"Error during cache cleanup: {e}")


    def _initialize_engine_and_callbacks(self, model_path: bytes, input_lang: bytes, output_lang: bytes):
        """Initialize or reinitialize the translation engine and callbacks."""

        print(f"[Instance {self.instance_id}] _initialize_engine_and_callbacks")

        cpp_cb = self._setup_engine_and_callback()

        # 4. Initialize the engine with the model and languages AFTER callback registration
        print(f"Initializing engine with model: {model_path}, input: {input_lang}, output: {output_lang}")
        init_ret = self._wrapper.translation_engine_init(
            self._engine,
            model_path,
            input_lang,
            output_lang
        )
        print(f"Engine init returned: {init_ret}")

        self._finish_init(
            init_ret,
            lambda: self._wrapper.translation_engine_init(
                self._engine, model_path, input_lang, output_lang
            ),
            cpp_cb
        )

        # Update current model parameters for this instance
        self.current_model_path = model_path
        self.current_input_lang = input_lang
        self.current_output_lang = output_lang

        # Update class-level tracking of what's loaded in the C++ engine
        TranslationWrapper._current_engine_model = model_path
        TranslationWrapper._current_engine_input_lang = input_lang
        TranslationWrapper._current_engine_output_lang = output_lang



    def _load_library(self, lib_path=None):
        """Load the translation shared library.
        
        Args:
            lib_path: Optional path to libtranslation_wrapper.so
            
        Returns:
            Loaded ctypes.CDLL library object
        """
        if lib_path is None:
            # Auto-detect library path
            lib_dir = os.path.abspath(os.path.dirname(__file__))
            primary_path = os.path.join(lib_dir, "libtranslation_wrapper.so")
            server_path = "/usr/src/server/libtranslation_wrapper.so"
            fallback_path = "/usr/lib/libtranslation_wrapper.so"

            if os.path.isfile(primary_path):
                lib_path = primary_path
                print("using lib_path")
            elif os.path.isfile(server_path):
                lib_path = server_path
                print("using server_path")
            elif os.path.isfile(fallback_path):
                lib_path = fallback_path
                print("using os_path")
            else:
                print("Error: libtranslation_wrapper.so not found!!!")
                sys.stderr.write(
                    f"Error: libtranslation_wrapper.so not found at {primary_path}, {server_path}, or {fallback_path}\n"
                )
                raise FileNotFoundError("libtranslation_wrapper.so not found")
        else:
            # If explicit path provided but doesn't exist, raise
            if not os.path.isfile(lib_path):
                sys.stderr.write(f"Error: {lib_path} not found\n")
                raise FileNotFoundError(lib_path)
        
        print(f"ctypes.CDLL{lib_path}")
        return ctypes.CDLL(lib_path)
    
    def _setup_c_api(self, wrapper):
        """Set up C API function signatures.
        
        Args:
            wrapper: The ctypes.CDLL library object to configure
        """
        # CTranslationCallback* create_translation_callback(OnResultFn, OnDoneFn,
        #                                                   OnErrorFn, void* user_data);
        wrapper.create_translation_callback.argtypes = [OnResultFn, OnDoneFn, OnErrorFn, ctypes.c_void_p]
        wrapper.create_translation_callback.restype = ctypes.c_void_p  # opaque pointer

        # void destroy_translation_callback(CTranslationCallback* cb);
        wrapper.destroy_translation_callback.argtypes = [ctypes.c_void_p]
        wrapper.destroy_translation_callback.restype = None

        # void* get_translation_result_callback(CTranslationCallback* cb);
        wrapper.get_translation_result_callback.argtypes = [ctypes.c_void_p]
        wrapper.get_translation_result_callback.restype = ctypes.c_void_p

        # CTranslationEngine* create_translation_engine(void);
        wrapper.create_translation_engine.argtypes = []
        wrapper.create_translation_engine.restype = ctypes.c_void_p
        
        # void destroy_translation_engine(CTranslationEngine* engine);
        wrapper.destroy_translation_engine.argtypes = [ctypes.c_void_p]
        wrapper.destroy_translation_engine.restype = None

        # int translation_engine_init(CTranslationEngine* engine,
        #                             const char* model_path,
        #                             const char* input_language,
        #                             const char* output_language);
        wrapper.translation_engine_init.argtypes = [ctypes.c_void_p,
                                                   ctypes.c_char_p,
                                                   ctypes.c_char_p,
                                                   ctypes.c_char_p]
        wrapper.translation_engine_init.restype = ctypes.c_int

        # void translation_engine_register_callback(CTranslationEngine* engine,
        #                                          void* c_callback);
        wrapper.translation_engine_register_callback.argtypes = [ctypes.c_void_p,
                                                               ctypes.c_void_p]
        wrapper.translation_engine_register_callback.restype = None

        # int translation_engine_process(CTranslationEngine* engine,
        #                               const char* source_text);
        wrapper.translation_engine_process.argtypes = [ctypes.c_void_p,
                                                     ctypes.c_char_p]
        wrapper.translation_engine_process.restype = ctypes.c_int

        # void translation_engine_stop(CTranslationEngine* engine);
        wrapper.translation_engine_stop.argtypes = [ctypes.c_void_p]
        wrapper.translation_engine_stop.restype = None

        # void translation_engine_deinit(CTranslationEngine* engine);
        wrapper.translation_engine_deinit.argtypes = [ctypes.c_void_p]
        wrapper.translation_engine_deinit.restype = None

        wrapper.translation_engine_init_from_buffer.argtypes = [ctypes.c_void_p,
                                           ctypes.c_void_p,
                                           ctypes.c_size_t,
                                           ctypes.c_char_p,
                                           ctypes.c_char_p]
                                                                

        wrapper.translation_engine_init_from_buffer.restype = ctypes.c_int

    def process(self, source_text: str) -> None:
        """Send a source text to the engine for translation.

        The result will be printed by the registered callback.
        """
        print(f"Processing text: {source_text}")
        self.final_text.clear()  # Clear previous results
        
        ret = self._wrapper.translation_engine_process(self._engine, source_text.encode('utf-8'))
        print(f"Process returned: {ret}")
        
        if ret != 0:
            print(f"Process failed with code {ret}")
            return None
            
        # Wait a bit for the callback to be called
        time.sleep(1.0)
        
        if self.final_text:
            print(f"Final text results: {self.final_text}")
            return self.final_text
        else:
            print("No results received from translation engine")
            return None

    def _ensure_correct_model(self, expected_model_path: bytes, expected_input_lang: bytes, expected_output_lang: bytes):
        """Ensure the engine is using the correct model, reinit if needed."""
        # Check against the class-level current engine state, not this instance's stored values
        if (TranslationWrapper._current_engine_model == expected_model_path and 
            TranslationWrapper._current_engine_input_lang == expected_input_lang and 
            TranslationWrapper._current_engine_output_lang == expected_output_lang):
            print(f"[Instance {self.instance_id}] Engine already has correct model, no reinit needed")
            print(f"[Instance {self.instance_id}] Engine model: {TranslationWrapper._current_engine_model}")
            return True
            
        print(f"[Instance {self.instance_id}] Model change detected, reinitializing...")
        print(f"[Instance {self.instance_id}] Engine currently has: {TranslationWrapper._current_engine_model} {TranslationWrapper._current_engine_input_lang}->{TranslationWrapper._current_engine_output_lang}")
        print(f"[Instance {self.instance_id}] Need to load: {expected_model_path} {expected_input_lang}->{expected_output_lang}")
        
        try:
            # Stop and deinit
            self._wrapper.translation_engine_stop(self._engine)
            self._wrapper.translation_engine_deinit(self._engine)
            
            # Reinit with new model
            cpp_cb = self._wrapper.get_translation_result_callback(TranslationWrapper._shared_cb_handle)
            self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
            
            init_ret = self._wrapper.translation_engine_init(
                self._engine, expected_model_path, expected_input_lang, expected_output_lang
            )
            
            if init_ret != 0:
                print(f"[Instance {self.instance_id}] ERROR: Reinit failed with code {init_ret}")
                return False
                
            self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
            
            # Update stored model info for this instance
            self.current_model_path = expected_model_path
            self.current_input_lang = expected_input_lang
            self.current_output_lang = expected_output_lang
            
            # Update class-level tracking of what's loaded in the C++ engine
            TranslationWrapper._current_engine_model = expected_model_path
            TranslationWrapper._current_engine_input_lang = expected_input_lang
            TranslationWrapper._current_engine_output_lang = expected_output_lang
            
            print(f"[Instance {self.instance_id}] Model change successful")
            print(f"[Instance {self.instance_id}] Engine now has: {TranslationWrapper._current_engine_model}")
            
            # Add delay to let DSP resources settle after model change
            print(f"[Instance {self.instance_id}] Waiting for DSP to settle after model change...")
            time.sleep(2.0)
            print(f"[Instance {self.instance_id}] DSP settle delay complete")
            
            return True
            
        except Exception as e:
            print(f"[Instance {self.instance_id}] ERROR during model change: {e}")
            return False
    
    def process_with_cb(self, source_text: str, on_result_cb, on_done_cb=None, on_error_cb=None, timeout=10.0):
        """Process text with custom callback functions.
        
        Args:
            source_text: Text to translate
            on_result_cb: Callback function(result_text: str) called when translation result is ready
            on_done_cb: Optional callback function() called when translation is complete
            on_error_cb: Optional callback function(error_code: int) called on error
            timeout: Maximum time to wait for translation (seconds)
            
        Returns:
            0 on success, non-zero error code on failure
        """
        print(f"[Instance {self.instance_id}] process_with_cb called for model: {self.current_model_path}")
        print(f"[Instance {self.instance_id}] Languages: {self.current_input_lang} -> {self.current_output_lang}")
        
        # Storage for error codes and state tracking
        error_code = [None]
        callback_state = {'result_received': False, 'done_received': False}
        
        # Create wrapper functions with additional safety checks
        def _wrapped_on_result(result):
            try:
                print(f"[Instance {self.instance_id}] _wrapped_on_result called")
                print(f"[Instance {self.instance_id}] Translation result: {result}")
                callback_state['result_received'] = True
                
                # Safety check for result
                if result is None:
                    print(f"[Instance {self.instance_id}] Warning: Received None result in callback")
                    return
                    
                if on_result_cb:
                    # Ensure we're passing a valid string
                    try:
                        if isinstance(result, bytes):
                            result = result.decode('utf-8')
                        elif not isinstance(result, str):
                            result = str(result)
                        on_result_cb(result)
                    except Exception as e:
                        print(f"[Instance {self.instance_id}] Error in user result callback: {e}")
                        traceback.print_exc()
            except Exception as e:
                print(f"[Instance {self.instance_id}] Error in result callback wrapper: {e}")
                traceback.print_exc()
        
        def _wrapped_on_done():
            try:
                print(f"[Instance {self.instance_id}] _wrapped_on_done called")
                callback_state['done_received'] = True
                if on_done_cb:
                    on_done_cb()
            except Exception as e:
                print(f"[Instance {self.instance_id}] Error in done callback: {e}")
                traceback.print_exc()
        
        def _wrapped_on_error(err_code):
            try:
                print(f"[Instance {self.instance_id}] _wrapped_on_error called with code: {err_code}")
                error_code[0] = err_code
                if on_error_cb:
                    on_error_cb(err_code)
            except Exception as e:
                print(f"[Instance {self.instance_id}] Error in error callback: {e}")
                traceback.print_exc()
        
        # Just ensure callback is registered before processing
        print(f"[Instance {self.instance_id}] Ensuring callback is registered before processing...")
        try:
            cpp_cb = self._wrapper.get_translation_result_callback(TranslationWrapper._shared_cb_handle)
            if not cpp_cb:
                print(f"[Instance {self.instance_id}] ERROR: Got NULL C++ callback pointer")
                return -1
            self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
            print(f"[Instance {self.instance_id}] Callback registered successfully")
        except Exception as e:
            print(f"[Instance {self.instance_id}] ERROR registering callback: {e}")
            return -1
        
        # Update the global callback manager to route to our custom handlers for this processing
        print(f"[Instance {self.instance_id}] Setting custom handlers for processing")
        _global_translation_callback_manager.set_handlers(
            on_result=_wrapped_on_result,
            on_done=_wrapped_on_done,
            on_error=_wrapped_on_error
        )
        
        try:
            # Verify engine is valid before processing
            if not self._engine:
                print(f"[Instance {self.instance_id}] Error: Translation engine is None, cannot process text")
                return -1
                
            # Process the text (callbacks will be routed through global manager)
            print(f"[Instance {self.instance_id}] Processing text: {source_text[:50]}...")
            print(f"[Instance {self.instance_id}] Input text: {source_text}")
            print(f"[Instance {self.instance_id}] Expected translation: {self.current_input_lang.decode()} -> {self.current_output_lang.decode()}")
            
            # Ensure text is properly encoded
            encoded_text = source_text.encode('utf-8') if isinstance(source_text, str) else source_text
            
            print(f"[Instance {self.instance_id}] About to call translation_engine_process...")
            print(f"[Instance {self.instance_id}] Engine handle: {self._engine}")
            print(f"[Instance {self.instance_id}] Encoded text length: {len(encoded_text)}")
            
            ret = self._wrapper.translation_engine_process(self._engine, encoded_text)
            print(f"[Instance {self.instance_id}] Process returned: {ret}")
            
            if ret != 0:
                print(f"[Instance {self.instance_id}] Process failed with return code: {ret}")
                return ret
            
            # If we get here with ret == 0, translation is complete!
            print(f"[Instance {self.instance_id}] Translation completed successfully (process returned 0)")
            
            # Wait a bit longer to ensure callbacks are called
            # The C++ code processes in chunks and calls callbacks for each chunk
            print(f"[Instance {self.instance_id}] Waiting for callbacks to complete...")
            max_wait = 5.0  # Maximum 5 seconds
            wait_interval = 0.1
            elapsed = 0.0
            
            while elapsed < max_wait:
                if callback_state['done_received']:
                    print(f"[Instance {self.instance_id}] Done callback received after {elapsed:.1f}s")
                    break
                time.sleep(wait_interval)
                elapsed += wait_interval
            
            if not callback_state['done_received']:
                print(f"[Instance {self.instance_id}] WARNING: Done callback not received after {elapsed:.1f}s")
            
            # Check if there was an error
            if error_code[0] is not None:
                print(f"[Instance {self.instance_id}] Error code received: {error_code[0]}")
                return error_code[0]
            
            # Log callback state for debugging
            print(f"[Instance {self.instance_id}] Callback state: result_received={callback_state['result_received']}, done_received={callback_state['done_received']}")
            
            if not callback_state['result_received']:
                print(f"[Instance {self.instance_id}] WARNING: No result callback received!")
                return -1
            
            print(f"[Instance {self.instance_id}] Processing completed successfully")
            return 0
            
        finally:
            # Restore this instance's default handlers
            print(f"[Instance {self.instance_id}] Restoring default instance handlers")
            _global_translation_callback_manager.set_handlers(
                on_result=self._instance_on_result,
                on_done=self._instance_on_done,
                on_error=self._instance_on_error
            )

    def close(self) -> None:
        """Clean up resources associated with the engine and callbacks."""
        
        print(f"[Instance {self.instance_id}] Starting translation wrapper cleanup...")
        
        # Step 1: Clear handlers from global callback manager only if this is the active instance
        try:
            print(f"[Instance {self.instance_id}] Clearing callback handlers from global manager...")
            _global_translation_callback_manager.clear_handlers()
            print(f"[Instance {self.instance_id}] Callback handlers cleared")
        except Exception as e:
            print(f"[Instance {self.instance_id}] Error clearing callback handlers: {e}")
            traceback.print_exc()

        # Step 2: Stop the engine
        try:
            if hasattr(self, '_engine') and self._engine:
                print(f"[Instance {self.instance_id}] Stopping translation engine...")
                self._wrapper.translation_engine_stop(self._engine)
                time.sleep(0.2)
                print(f"[Instance {self.instance_id}] Engine stopped")
        except Exception as e:
            print(f"[Instance {self.instance_id}] Error stopping engine: {e}")
            traceback.print_exc()

        # Step 3: Free the wrapper handle
        # Note: destroy_translation_engine will call deInit() on this instance's engine
        try:
            if hasattr(self, '_engine') and self._engine:
                print(f"[Instance {self.instance_id}] Destroying translation engine wrapper...")
                self._wrapper.destroy_translation_engine(self._engine)
                self._engine = None
                print(f"[Instance {self.instance_id}] Engine wrapper destroyed")
        except Exception as e:
            print(f"[Instance {self.instance_id}] Error destroying engine wrapper: {e}")
            traceback.print_exc()
        
        # Step 4: Note - We do NOT destroy the global callback handle
        # The global callback handle is shared across all instances and should never be destroyed
        # It uses the global C callbacks which must remain valid for the lifetime of the application
        print(f"[Instance {self.instance_id}] Note: Global callback handle is preserved (shared across all instances)")
        
        print(f"[Instance {self.instance_id}] Translation wrapper cleanup finished")


    def change_model(self, model_path: bytes, input_lang: bytes, output_lang: bytes) -> bool:
        """Change the translation model and languages.
        
        This method reinitializes the singleton engine with a new model.
        If the current model is the same as the requested one, it does nothing.
        
        Args:
            model_path: Path to the new model file
            input_lang: New input language
            output_lang: New output language
            
        Returns:
            True if model was changed or already matched the requested one,
            False if there was an error changing the model
        """
        # Check if we're already using this model and languages
        if (self.current_model_path == model_path and 
            self.current_input_lang == input_lang and 
            self.current_output_lang == output_lang):
            print("Model and languages already match the requested ones, no change needed")
            return True
            
        print(f"Changing model from {self.current_model_path} to {model_path}")
        print(f"Changing languages from {self.current_input_lang}->{self.current_output_lang} to {input_lang}->{output_lang}")
        
        try:
            # First stop the current translation process
            if hasattr(self, '_engine') and self._engine:
                print("Stopping translation engine...")
                self._wrapper.translation_engine_stop(self._engine)
                # Add longer delay to ensure engine is fully stopped
                time.sleep(0.5)
                
                # Clear any pending callbacks before deinit
                print("Clearing callback handlers before deinit...")
                _global_translation_callback_manager.clear_handlers()
                time.sleep(0.2)
                
                print("Deinitializing translation engine...")
                self._wrapper.translation_engine_deinit(self._engine)
                # Add longer delay to ensure deinit completes fully
                time.sleep(0.5)
            
            # Since we're dealing with a singleton, we need to reinitialize it with the new model
            # rather than trying to destroy and recreate it
            
            # CRITICAL: Re-register the callback BEFORE reinitializing
            # The C++ init() function checks mResultCallback for error reporting
            print("Re-registering global callback with engine BEFORE reinit...")
            cpp_cb = self._wrapper.get_translation_result_callback(TranslationWrapper._shared_cb_handle)
            print(f"Got C++ callback pointer: {cpp_cb}")
            self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
            print("Callback re-registered successfully BEFORE reinit")
            
            print(f"Reinitializing engine with model: {model_path}, input: {input_lang}, output: {output_lang}")            
            init_ret = self._wrapper.translation_engine_init(
                self._engine,
                model_path,
                input_lang,
                output_lang
            )
            print(f"Engine init returned: {init_ret}")
            
            # CRITICAL: Re-register the callback AFTER reinit as well
            # The init() function might reset internal state
            print("Re-registering callback AFTER reinit to ensure it's set...")
            self._wrapper.translation_engine_register_callback(self._engine, cpp_cb)
            print("Callback re-registered successfully AFTER reinit")
            
            # Restore default handlers
            _global_translation_callback_manager.set_handlers(
                on_result=self._instance_on_result,
                on_done=self._instance_on_done,
                on_error=self._instance_on_error
            )

            if init_ret != 0:
                print(f"ERROR: Engine initialization failed with code {init_ret}")
                print("The C++ wrapper already tried to recover, but init still failed")
                print("Model change failed")
                return False
            
            # Update current model parameters
            self.current_model_path = model_path
            self.current_input_lang = input_lang
            self.current_output_lang = output_lang
            
            print("Model changed successfully")
            return True
        except Exception as e:
            print(f"Error changing model: {e}")
            traceback.print_exc()
            return False
    
    def __del__(self):
        # Ensure resources are released if close() wasn't called explicitly
        self.close()
