# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import ctypes
import os
import socket
import time
import sys

from ctypes import (
    CDLL, c_void_p, c_char_p, c_bool, c_int32, POINTER,
    c_uint8, CFUNCTYPE, Structure
)

# Define the C structure for transcription key-value pairs
class WhisperTranscriptionKV(Structure):
    _fields_ = [
        ("key", c_char_p),
        ("value", c_char_p),
    ]

class WhisperWrapper:
    # Class-level variable to store the loaded library (shared across all instances)
    _shared_lib = None
    _library_loaded = False
    
    def __init__(
        self,
        language: None,
        translation_enabled: bool = False,
        continuous: bool = True,
        partial_transcriptions: bool = False,
        min_buffer_ms: int = 2000,
        on_transcription=None,   # callable(dict) or None
        on_event=None,           # callable(int) or None
        on_error=None,           # callable(int) or None
        lib_path: str = None
    ):
        """
        Initialize WhisperWrapper.
        
        Args:
            language: Language code (default: None)
            translation_enabled: Enable translation (default: False)
            continuous: Enable continuous transcription (default: True)
            min_buffer_ms: Minimum buffer in milliseconds (default: 200)
            on_transcription: Callback for transcription results
            on_event: Callback for events
            on_error: Callback for errors
            lib_path: Path to libwhisperwrapper.so (optional, auto-detected if None)
        """
        # Load library only once (class-level)
        if not WhisperWrapper._library_loaded:
            WhisperWrapper._shared_lib = self._load_library(lib_path)
            self._setup_c_api(WhisperWrapper._shared_lib)
            WhisperWrapper._library_loaded = True
            print("Whisper library loaded for the first time")
        else:
            print("Reusing already loaded Whisper library")
        
        # Use the shared library instance
        self.lib = WhisperWrapper._shared_lib       

        # Handles
        self.handle = None
        self.stream = None
        self.wr_listener = None
        self.data_listener = None

        self._TRANSCRIPTION_CB = on_transcription
        self._EVENT_CB = on_event
        self._ERROR_CB = on_error
        
        # Config
        self.language = language
        self.translation_enabled = translation_enabled
        self.continuous = continuous
        self.partial_transcriptions = partial_transcriptions
        self.min_buffer_ms = min_buffer_ms

        # Event / error constants
        self.KEY_TRANSCRIPTION = self.lib.whisper_response_listener_KEY_TRANSCRIPTION().decode()
        self.KEY_LANGUAGE      = self.lib.whisper_response_listener_KEY_LANGUAGE().decode()
        self.KEY_IS_FINAL      = self.lib.whisper_response_listener_KEY_IS_FINAL().decode()

        self.EVENT_SPEECH_STARTED = self.lib.whisper_response_listener_EVENT_SPEECH_STARTED()
        self.EVENT_SPEECH_ENDED   = self.lib.whisper_response_listener_EVENT_SPEECH_ENDED()
        self.ERROR_NO_SPEECH_TIMEOUT = self.lib.whisper_response_listener_ERROR_NO_SPEECH_TIMEOUT()

    # -------------------------------------------------------------------------
    # Library loading
    # -------------------------------------------------------------------------
    def _load_library(self, lib_path=None):
        """Load the Whisper shared library.
        
        Args:
            lib_path: Optional path to libwhisperwrapper.so
            
        Returns:
            Loaded ctypes.CDLL library object
        """
        if lib_path is None:
            # Auto-detect library path
            lib_dir = os.path.abspath(os.path.dirname(__file__))
            lib_path = os.path.join(lib_dir, "libwhisperwrapper.so")
            
            # If not found in engine/python, try /usr/src/server (where libraries are now located)
            if not os.path.isfile(lib_path):
                lib_path = "/usr/src/server/libwhisperwrapper.so"
            
            # If still not found, try /usr/lib as fallback
            if not os.path.isfile(lib_path):
                lib_path = "/usr/lib/libwhisperwrapper.so"
        
        if not os.path.isfile(lib_path):
            sys.stderr.write(f"Error: {lib_path} not found\n")
            raise FileNotFoundError(lib_path)
        
        return ctypes.CDLL(lib_path)

    # -------------------------------------------------------------------------
    # C API setup
    # -------------------------------------------------------------------------
    def _setup_c_api(self, lib):
        """Set up C API function signatures.
        
        Args:
            lib: The ctypes.CDLL library object to configure
        """

        # Create / init / deinit
        lib.whisper_create.restype = c_void_p

        lib.whisper_init.argtypes = [
            c_void_p,
            c_char_p, c_char_p,
            c_char_p, c_char_p, c_char_p
        ]
        lib.whisper_init.restype = c_bool

        lib.whisper_deinit.argtypes = [c_void_p]
        lib.whisper_deinit.restype = None

        lib.whisper_destroy.argtypes = [c_void_p]
        lib.whisper_destroy.restype = None

        # Some setters used in your code but not defined there:
        # These must exist in the .so; argtypes are guessed from usage
        lib.whisper_set_partial_transcriptions_enabled.argtypes = [c_void_p, c_bool]
        lib.whisper_set_partial_transcriptions_enabled.restype = None

        lib.whisper_set_min_buffer_ms.argtypes = [c_void_p, c_int32]
        lib.whisper_set_min_buffer_ms.restype = None

        lib.whisper_set_vad_len_hangover.argtypes = [c_void_p, c_int32]
        lib.whisper_set_vad_len_hangover.restype = None

        lib.whisper_get_vad_len_hangover.argtypes = [c_void_p]
        lib.whisper_get_vad_len_hangover.restype = c_int32

        lib.whisper_stop.argtypes = [c_void_p]
        lib.whisper_stop.restype = None

        lib.whisper_flush.argtypes = [c_void_p]
        lib.whisper_flush.restype = None

        # Language / translation
        lib.whisper_set_translation_enabled.argtypes = [c_void_p, c_bool]
        lib.whisper_set_translation_enabled.restype = None

        lib.whisper_set_language_code.argtypes = [c_void_p, c_char_p]
        lib.whisper_set_language_code.restype = None

        # Continuous mode
        lib.whisper_get_continuous_transcription_enabled.argtypes = [c_void_p]
        lib.whisper_get_continuous_transcription_enabled.restype = c_bool

        lib.whisper_set_continuous_transcription_enabled.argtypes = [c_void_p, c_bool]
        lib.whisper_set_continuous_transcription_enabled.restype = None

        # Input stream
        lib.input_stream_create.restype = c_void_p
        lib.input_stream_destroy.argtypes = [c_void_p]

        lib.input_stream_set_use_audio_file.argtypes = [c_void_p, c_bool]
        lib.input_stream_set_use_audio_file.restype = None

        lib.input_stream_write_file.argtypes = [c_void_p, c_char_p]
        lib.input_stream_write_file.restype = None

        lib.input_stream_write_buffer.argtypes = [c_void_p, POINTER(c_uint8), c_int32]
        lib.input_stream_write_buffer.restype = None

        lib.input_stream_register_data_available_listener.argtypes = [c_void_p, c_void_p]
        lib.input_stream_unregister_data_available_listener.argtypes = [c_void_p]

        lib.input_stream_get_use_audio_file.argtypes = [c_void_p]
        lib.input_stream_get_use_audio_file.restype = c_bool

        # Response listener
        lib.whisper_response_listener_create.restype = c_void_p
        lib.whisper_response_listener_destroy.argtypes = [c_void_p]
        
        # Callback setters
        lib.whisper_response_listener_set_on_transcription.argtypes = [c_void_p, c_void_p, c_void_p]
        lib.whisper_response_listener_set_on_transcription.restype = None
        
        lib.whisper_response_listener_set_on_event.argtypes = [c_void_p, c_void_p, c_void_p]
        lib.whisper_response_listener_set_on_event.restype = None
        
        lib.whisper_response_listener_set_on_error.argtypes = [c_void_p, c_void_p, c_void_p]
        lib.whisper_response_listener_set_on_error.restype = None

        lib.whisper_start.argtypes = [c_void_p, c_void_p]
        lib.whisper_start.restype = None

        lib.whisper_register_listener.argtypes = [c_void_p, c_void_p]
        lib.whisper_register_listener.restype = None

        # Optional string / int getters already used above
        lib.whisper_response_listener_KEY_TRANSCRIPTION.restype = c_char_p
        lib.whisper_response_listener_KEY_LANGUAGE.restype      = c_char_p
        lib.whisper_response_listener_KEY_IS_FINAL.restype      = c_char_p

        lib.whisper_response_listener_EVENT_SPEECH_STARTED.restype = c_int32
        lib.whisper_response_listener_EVENT_SPEECH_ENDED.restype   = c_int32
        lib.whisper_response_listener_ERROR_NO_SPEECH_TIMEOUT.restype = c_int32

    # -------------------------------------------------------------------------
    # Whisper lifecycle
    # -------------------------------------------------------------------------
    def _init_whisper(self,encoder_path,decoder_path,vocab_path,speech_path,model_path):
        # Initialize Whisper
        self.handle = self.lib.whisper_create()
        if not self.handle:
            raise RuntimeError("Failed to create whisper handle")
        
        ok = self.lib.whisper_init(
            self.handle,
            encoder_path,
            decoder_path,
            vocab_path,
            speech_path,
            model_path,
        )
        if not ok:
            raise RuntimeError("whisper_init failed")

        # Basic settings
        self.lib.whisper_set_partial_transcriptions_enabled(self.handle, self.partial_transcriptions)
        self.lib.whisper_set_continuous_transcription_enabled(self.handle, self.continuous)
        self.lib.whisper_set_min_buffer_ms(self.handle, self.min_buffer_ms)        
        print(f'language: {self.language}')
        self.lib.whisper_set_language_code(self.handle, self.language)
        self.lib.whisper_set_translation_enabled(self.handle, self.translation_enabled)

        # Response listener
        self.wr_listener = self.lib.whisper_response_listener_create()
        if self._TRANSCRIPTION_CB:
            self.lib.whisper_response_listener_set_on_transcription(self.wr_listener, self._TRANSCRIPTION_CB, self.handle)
        if self._EVENT_CB:
            self.lib.whisper_response_listener_set_on_event(self.wr_listener, self._EVENT_CB, self.handle)
        if self._ERROR_CB:
            self.lib.whisper_response_listener_set_on_error(self.wr_listener, self._ERROR_CB, self.handle)
        self.lib.whisper_register_listener(self.handle, self.wr_listener)

        # Input stream (initially not file)
        self.stream = self.lib.input_stream_create()
        if not self.stream:
            raise RuntimeError("Failed to create InputStream")

        self.lib.input_stream_set_use_audio_file(self.stream, False)

        if self.data_listener:
            self.lib.input_stream_register_data_available_listener(self.stream, self.data_listener)
        return self.handle

    def close(self):
        """Cleanly stop and destroy everything, ensuring all DSP resources are released."""
        print("Starting WhisperWrapper cleanup...")
        
        # First stop any ongoing processing
        if self.handle:
            try:
                print("Stopping Whisper processing...")
                self.lib.whisper_stop(self.handle)
                print("Whisper processing stopped successfully")
            except Exception as e:
                print(f"Error stopping Whisper processing: {e}")
                import traceback
                traceback.print_exc()

        # Clean up the response listener
        if self.wr_listener:
            try:
                print("Destroying response listener...")
                self.lib.whisper_response_listener_destroy(self.wr_listener)
                self.wr_listener = None
                print("Response listener destroyed successfully")
            except Exception as e:
                print(f"Error destroying response listener: {e}")
                import traceback
                traceback.print_exc()

        # Clean up the input stream
        if self.stream:
            try:
                print("Destroying input stream...")
                self.lib.input_stream_destroy(self.stream)
                self.stream = None
                print("Input stream destroyed successfully")
            except Exception as e:
                print(f"Error destroying input stream: {e}")
                import traceback
                traceback.print_exc()

        # Deinitialize the Whisper engine to release DSP resources
        if self.handle:
            try:
                import time
                print("Deinitializing Whisper engine...")
                self.lib.whisper_deinit(self.handle)
                print("Freeing Whisper engine...")
                self.lib.whisper_destroy(self.handle)
                self.handle = None
                print("Whisper engine deinitialized successfully")
            except Exception as e:
                print(f"Error deinitializing Whisper engine: {e}")
                import traceback
                traceback.print_exc()
        
        # Force garbage collection multiple times to ensure cleanup
        # try:
        #     import gc
        #     for i in range(3):
        #         gc.collect()
        #         print(f"Garbage collection pass {i+1} completed")
        # except Exception as e:
        #     print(f"Error during garbage collection: {e}")
        #     import traceback
        #     traceback.print_exc()
        
        # Add a small delay to allow DSP cleanup to complete
        try:
            import time
            time.sleep(0.2)  # Slightly longer delay for ASR cleanup
            print("DSP cleanup delay completed")
        except Exception as e:
            print(f"Error during cleanup delay: {e}")
        
        print("WhisperWrapper cleanup completed")

    def deInit(self):
        if self.handle:
            try:
                self.lib.whisper_deinit(self.handle)
            except:
                pass
        self.handle = None

    # -------------------------------------------------------------------------
    # Public API
    # -------------------------------------------------------------------------
    def start(self):
        """Start the Whisper processing loop."""
        print("Starting Whisper processing...")
        self.lib.whisper_start(self.handle, self.stream)

    def stop(self):
        print("Stopping Whisper...")
        self.lib.whisper_stop(self.handle)

    def flush(self):
        """Request the processing thread to drain the current buffer as a
        partial result (transcript.text.delta) without stopping the engine.
        The flag is set and mCv is notified so the thread wakes immediately.
        Returns as soon as the signal is sent - the actual processing happens
        asynchronously on the C++ processing thread.
        """
        if not self.handle:
            print("flush: no handle, ignoring")
            return
        print("Flushing Whisper buffer (partial)...")
        self.lib.whisper_flush(self.handle)
    
    def stop_and_reset(self):
        """Stop processing and reset for next use (for singleton pattern)."""
        self.stop()
        self.reset_for_reuse()
    
    def update_callbacks(self, on_transcription=None, on_event=None, on_error=None, partial_transcriptions=None):
        """Update callbacks and settings for the wrapper (for singleton reuse).
        
        Args:
            on_transcription: New transcription callback
            on_event: New event callback  
            on_error: New error callback
            partial_transcriptions: Enable/disable partial transcriptions (optional)
        """
        if not self.handle or not self.wr_listener:
            print("Cannot update callbacks - wrapper not initialized")
            return
        
        # Update partial_transcriptions setting if provided
        if partial_transcriptions is not None:
            self.partial_transcriptions = partial_transcriptions
            self.lib.whisper_set_partial_transcriptions_enabled(self.handle, partial_transcriptions)
            print(f"Updated partial_transcriptions to {partial_transcriptions}")
        
        # Update stored callbacks
        if on_transcription is not None:
            self._TRANSCRIPTION_CB = on_transcription
        if on_event is not None:
            self._EVENT_CB = on_event
        if on_error is not None:
            self._ERROR_CB = on_error
        
        # Destroy old listener
        self.lib.whisper_response_listener_destroy(self.wr_listener)
        
        # Create new listener with updated callbacks
        self.wr_listener = self.lib.whisper_response_listener_create()
        if self._TRANSCRIPTION_CB:
            self.lib.whisper_response_listener_set_on_transcription(self.wr_listener, self._TRANSCRIPTION_CB, self.handle)
        if self._EVENT_CB:
            self.lib.whisper_response_listener_set_on_event(self.wr_listener, self._EVENT_CB, self.handle)
        if self._ERROR_CB:
            self.lib.whisper_response_listener_set_on_error(self.wr_listener, self._ERROR_CB, self.handle)
        
        # Re-register listener
        self.lib.whisper_register_listener(self.handle, self.wr_listener)
        print("Callbacks updated successfully")
    
    def reset_for_reuse(self):
        """Reset the wrapper state for reuse (for singleton pattern).
        
        Destroys the old response listener and input stream and creates fresh
        empty replacements. Callbacks are intentionally NOT re-registered here
        — the old callbacks are closures from the previous request and must not
        be called again. update_callbacks() will register the new request's
        callbacks before the next whisper_start().
        """
        if not self.handle:
            print("Cannot reset - wrapper not initialized")
            return
        
        # Destroy old response listener. Clear stored callback refs so the
        # previous request's closures can be garbage collected and the C++
        # side cannot fire them after this point.
        if self.wr_listener:
            self.lib.whisper_response_listener_destroy(self.wr_listener)
            self.wr_listener = None
        self._TRANSCRIPTION_CB = None
        self._EVENT_CB = None
        self._ERROR_CB = None

        # Create a fresh empty listener — update_callbacks() will populate it.
        self.wr_listener = self.lib.whisper_response_listener_create()
        self.lib.whisper_register_listener(self.handle, self.wr_listener)
        print("Response listener reset (empty, awaiting update_callbacks)")

        # Destroy old input stream and recreate for clean state.
        if self.stream:
            self.lib.input_stream_destroy(self.stream)
        self.stream = self.lib.input_stream_create()
        if not self.stream:
            raise RuntimeError("Failed to create new InputStream")
        self.lib.input_stream_set_use_audio_file(self.stream, False)
        if self.data_listener:
            self.lib.input_stream_register_data_available_listener(self.stream, self.data_listener)
        
        print("Wrapper reset for reuse")

    def process_file(self, file_path: str):
        """
        Process a single audio file. `file_path` is a string path.
        """
        if not os.path.isfile(file_path):
            raise FileNotFoundError(file_path)
        
        self.lib.input_stream_set_use_audio_file(self.stream, True)
        self.lib.input_stream_write_file(self.stream, file_path.encode("utf-8"))
    

    def process_tcp_stream(self, host: str = "127.0.0.1", port: int = 5000):
        """
        Start a TCP server and feed incoming audio to Whisper.
        Blocking call.
        """
        self.lib.input_stream_set_use_audio_file(self.stream, False)

        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind((host, port))
        server_sock.listen(5)

        print(f"Server listening on {host}:{port}")
        try:
            while True:
                client_sock, address = server_sock.accept()
                print(f"Accepted connection from {address}")
                self._process_buffer(client_sock)
        finally:
            server_sock.close()
            print("Server socket closed")
            self.stop()

    # -------------------------------------------------------------------------
    # Internal helpers
    # -------------------------------------------------------------------------
    def set_vad_len_hangover(self, value: int):
        """Set VAD length hangover parameter.
        
        Args:
            value: VAD length hangover value in milliseconds
        """
        if self.handle:
            # Convert from milliseconds to 10ms frame units and ensure integer
            vad_frames = int(value / 10)
            self.lib.whisper_set_vad_len_hangover(self.handle, vad_frames)
            print(f"Set VAD length hangover to {value}ms ({vad_frames} frames)")
    
    def get_vad_len_hangover(self) -> int:
        """Get current VAD length hangover parameter.
        
        Returns:
            Current VAD length hangover value in milliseconds
        """
        if self.handle:
            return self.lib.whisper_get_vad_len_hangover(self.handle)
        return 0

    def _process_buffer(self, client_socket):
        try:
            while True:
                data = client_socket.recv(4096)
                if not data:
                    break
                buf = (c_uint8 * len(data)).from_buffer_copy(data)
                self.lib.input_stream_write_buffer(self.stream, buf, len(data))
        finally:
            client_socket.close()
            print("TCP connection closed")
