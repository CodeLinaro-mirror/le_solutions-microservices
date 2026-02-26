# Python Wrappers for Native Audio Engines

This directory contains Python wrappers for the native C/C++ audio processing engines.

Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.  
SPDX-License-Identifier: BSD-3-Clause-Clear

---

## Overview

These wrappers provide Python interfaces to the native audio processing libraries:

- **`asr_wrapper.py`** - Automatic Speech Recognition (Whisper)
- **`translate_wrapper.py`** - Text-to-Text Translation (Opus/NLLB)
- **`tts_wrapper.py`** - Text-to-Speech (MeloTTS)

Each wrapper uses `ctypes` to interface with the corresponding `.so` library in `../native/aarch64-oe-linux/`.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   Python Services                        │
│         (app/services/asr_service.py, etc.)             │
└────────────────────┬────────────────────────────────────┘
                     │
                     │ import & call
                     │
┌────────────────────▼────────────────────────────────────┐
│              Python Wrappers (this directory)            │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ asr_wrapper  │  │  translate_  │  │ tts_wrapper  │  │
│  │    .py       │  │  wrapper.py  │  │    .py       │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
│         │                  │                  │          │
│         │ ctypes           │ ctypes           │ ctypes   │
│         │                  │                  │          │
└─────────┼──────────────────┼──────────────────┼──────────┘
          │                  │                  │
          │                  │                  │
┌─────────▼──────────────────▼──────────────────▼──────────┐
│           Native Libraries (.so files)                    │
│  ┌──────────────────┐  ┌──────────────────┐             │
│  │ libwhisper       │  │ libtranslation   │             │
│  │ wrapper.so       │  │ _wrapper.so      │             │
│  └──────────────────┘  └──────────────────┘             │
│  ┌──────────────────┐                                    │
│  │ libtts_c         │                                    │
│  │ _wrapper.so      │                                    │
│  └──────────────────┘                                    │
│                                                           │
│  Location: ../native/aarch64-oe-linux/                   │
│  Copied to: /usr/lib/ (in Docker)                        │
└───────────────────────────────────────────────────────────┘
```

---

## Library Loading

All wrappers automatically locate their corresponding `.so` files:

1. **First**, check the same directory as the wrapper: `engine/python/`
2. **Then**, check the system library path: `/usr/lib/`

This allows the wrappers to work both:
- In development (when `.so` files are in `../native/aarch64-oe-linux/`)
- In production (when `.so` files are copied to `/usr/lib/` by Docker)

---

## ASR Wrapper (`asr_wrapper.py`)

### Purpose
Provides Python interface to the Whisper ASR engine for speech-to-text transcription.

### Native Library
- **Library**: `libwhisperwrapper.so`
- **Dependencies**: `libwhisperfunction.so`, `libwhisper_lib.so`, `libdnnvad.so`, `libfft.so`

### Key Features
- File-based transcription
- Real-time streaming transcription
- TCP socket streaming
- Continuous transcription mode
- Multi-language support
- Translation support

### Usage Example

```python
from asr_wrapper import WhisperWrapper
from ctypes import CFUNCTYPE, POINTER, c_char_p, c_int32, c_void_p

# Define callback for transcription results
@CFUNCTYPE(None, POINTER(WhisperTranscriptionKV), c_int32, c_void_p)
def on_transcription(results, count, user_data):
    for i in range(count):
        kv = results[i]
        key = kv.key.decode("utf-8")
        value = kv.value.decode("utf-8")
        print(f"{key}: {value}")

# Create wrapper
wrapper = WhisperWrapper(
    language=b"en",
    translation_enabled=False,
    continuous=True,
    min_buffer_ms=200,
    on_transcription=on_transcription
)

# Initialize with model paths
wrapper._init_whisper(
    encoder_path=b"/path/to/encoder.qnn",
    decoder_path=b"/path/to/decoder.qnn",
    vocab_path=b"/path/to/vocab.bin",
    speech_path=b"/path/to/speech.bin",
    model_path=b""
)

# Start processing
wrapper.start()

# Process a file
wrapper.process_file("/path/to/audio.wav")

# Or process streaming audio
# wrapper.process_tcp_stream(host="127.0.0.1", port=5000)

# Clean up
wrapper.close()
```

### Key Methods

| Method | Description |
|--------|-------------|
| `__init__(...)` | Initialize wrapper with configuration |
| `_init_whisper(...)` | Initialize Whisper engine with model paths |
| `start()` | Start the processing loop |
| `stop()` | Stop processing |
| `process_file(path)` | Process an audio file |
| `process_tcp_stream(host, port)` | Start TCP server for streaming |
| `close()` | Clean up resources |

### Callbacks

- **`on_transcription`**: Called with transcription results (key-value pairs)
- **`on_event`**: Called for events (speech started/ended)
- **`on_error`**: Called on errors (no speech timeout, etc.)

### Configuration

```python
WhisperWrapper(
    language=b"en",              # Language code (en, zh, es, etc.)
    translation_enabled=False,   # Enable translation to English
    continuous=True,             # Continuous transcription mode
    min_buffer_ms=200,          # Minimum buffer size in milliseconds
    on_transcription=callback,  # Transcription callback
    on_event=None,              # Event callback (optional)
    on_error=None,              # Error callback (optional)
    lib_path=None               # Custom library path (optional)
)
```

---

## Translation Wrapper (`translate_wrapper.py`)

### Purpose
Provides Python interface to the Opus/NLLB translation engine for text-to-text translation.

### Native Library
- **Library**: `libtranslation_wrapper.so`
- **Dependencies**: `libtranslation.so`, `libtranslation_skel.so`

### Key Features
- Text-to-text translation
- Multiple language pairs
- Callback-based results
- Synchronous processing

### Usage Example

```python
from translate_wrapper import TranslationWrapper

# Create wrapper
wrapper = TranslationWrapper(
    model_path=b"/path/to/opus_zh_to_en.qnn",
    input_lang=b"chinese",
    output_lang=b"english"
)

# Process text (uses default callbacks)
wrapper.process("示例文本")

# Or use custom callbacks
def on_result(result_text):
    print(f"Translation: {result_text}")

def on_done():
    print("Translation complete")

def on_error(error_code):
    print(f"Error: {error_code}")

wrapper.process_with_cb(
    "示例文本",
    on_result_cb=on_result,
    on_done_cb=on_done,
    on_error_cb=on_error
)

# Clean up
wrapper.close()
```

### Key Methods

| Method | Description |
|--------|-------------|
| `__init__(model_path, input_lang, output_lang)` | Initialize with model and languages |
| `process(text)` | Process text with default callbacks |
| `process_with_cb(text, callbacks...)` | Process with custom callbacks |
| `close()` | Clean up resources |

### Callbacks

- **`on_result`**: Called with translation result text
- **`on_done`**: Called when translation is complete
- **`on_error`**: Called on error with error code

### Supported Languages

Common language codes:
- `b"english"` / `b"en"`
- `b"chinese"` / `b"zh"`
- `b"spanish"` / `b"es"`
- `b"french"` / `b"fr"`

---

## TTS Wrapper (`tts_wrapper.py`)

### Purpose
Provides Python interface to the MeloTTS engine for text-to-speech synthesis.

### Native Library
- **Library**: `libtts_c_wrapper.so`
- **Dependencies**: `libeai_floating.so`

### Key Features
- Text-to-speech synthesis
- Streaming audio output via callbacks
- File output support
- Configurable voice parameters
- Multiple languages

### Usage Example

```python
from tts_wrapper import TTS
import ctypes

# Create TTS instance
tts = TTS()

# Initialize with model
handle = tts.init(
    model_location=b"/path/to/melo_en.qnn",
    audio_encoding=0,
    speaking_rate=1.0,
    pitch=0.0,
    volume_gain=0.0,
    sample_rate=44100,
    language_code=0  # 0=English, 1=Chinese, 2=Spanish, etc.
)

# Option 1: Process to file
result = tts.process_to_file(
    text=b"Hello, world!",
    output_file=b"output.wav"
)

# Option 2: Process with callback for streaming
@TTS.CHUNK_CALLBACK_TYPE
def chunk_callback(pcm_data, pcm_size):
    chunk = ctypes.string_at(pcm_data, pcm_size)
    # Process audio chunk (e.g., stream to client)
    print(f"Received {pcm_size} bytes")

result = tts.process_with_callback(
    text=b"Hello, world!",
    callback_function=chunk_callback
)

# Clean up
tts.deinit()
```

### Key Methods

| Method | Description |
|--------|-------------|
| `__init__(lib_path=None)` | Initialize wrapper |
| `init(model_location, ...)` | Initialize TTS engine with model |
| `process_to_file(text, output_file)` | Synthesize to file |
| `process_with_callback(text, callback)` | Synthesize with streaming callback |
| `create_chunk_callback()` | Helper to create a chunk callback |
| `deinit()` | Clean up resources |

### Configuration

```python
tts.init(
    model_location=b"/path/to/model.qnn",  # Model file path
    audio_encoding=0,                       # Audio encoding format
    speaking_rate=1.0,                      # Speed (0.5-2.0)
    pitch=0.0,                              # Pitch adjustment
    volume_gain=0.0,                        # Volume adjustment (dB)
    sample_rate=44100,                      # Sample rate (Hz)
    language_code=0                         # Language (0=EN, 1=ZH, 2=ES, etc.)
)
```

### Language Codes

| Code | Language |
|------|----------|
| 0 | English |
| 1 | Chinese |
| 2 | Spanish |
| 3 | French |

---

## Common Patterns

### Error Handling

All wrappers should be used with proper error handling:

```python
try:
    wrapper = WhisperWrapper(...)
    wrapper._init_whisper(...)
    wrapper.start()
    # ... processing ...
except Exception as e:
    print(f"Error: {e}")
finally:
    wrapper.close()
```

### Resource Management

Always call `close()` or `deinit()` to free resources:

```python
# Using try/finally
wrapper = WhisperWrapper(...)
try:
    # ... use wrapper ...
    pass
finally:
    wrapper.close()

# Or using context manager (if implemented)
with WhisperWrapper(...) as wrapper:
    # ... use wrapper ...
    pass
```

### Callback Functions

Callbacks must match the C function signature:

```python
from ctypes import CFUNCTYPE, c_void_p, c_char_p, c_int32

# Define callback type
CallbackType = CFUNCTYPE(None, c_void_p, c_char_p)

# Create callback function
@CallbackType
def my_callback(user_data, result):
    # Process result
    pass

# Pass to wrapper
wrapper.set_callback(my_callback)
```

---

## Integration with Services

The wrappers are used by the services in `app/services/`:

### ASR Service Integration

```python
# In app/services/asr_service.py
import importlib.util
import sys

# Load wrapper dynamically
wrapper_path = "/usr/src/engine/python/asr_wrapper.py"
spec = importlib.util.spec_from_file_location("asr_wrapper", wrapper_path)
asr_wrapper_module = importlib.util.module_from_spec(spec)
sys.modules["asr_wrapper"] = asr_wrapper_module
spec.loader.exec_module(asr_wrapper_module)

# Use wrapper
wrapper = asr_wrapper_module.WhisperWrapper(...)
```

### Translation Service Integration

```python
# In app/services/t2t_service.py
from engine.python.translate_wrapper import TranslationWrapper

wrapper = TranslationWrapper(
    model_path=b"/path/to/model.qnn",
    input_lang=b"chinese",
    output_lang=b"english"
)
```

### TTS Service Integration

```python
# In app/services/tts_service.py
from engine.python.tts_wrapper import TTS

tts = TTS()
tts.init(b"/path/to/model.qnn", sample_rate=44100, language_code=0)
```

---

## Native Library Dependencies

### Required .so Files

All wrappers depend on native libraries in `../native/aarch64-oe-linux/`:

| Wrapper | Primary Library | Dependencies |
|---------|----------------|--------------|
| ASR | `libwhisperwrapper.so` | `libwhisperfunction.so`, `libwhisper_lib.so`, `libdnnvad.so`, `libfft.so` |
| Translation | `libtranslation_wrapper.so` | `libtranslation.so`, `libtranslation_skel.so` |
| TTS | `libtts_c_wrapper.so` | `libeai_floating.so` |

### Library Loading Path

The wrappers search for libraries in this order:

1. Same directory as wrapper: `engine/python/`
2. System library path: `/usr/lib/`

In Docker, all `.so` files are copied to `/usr/lib/` during build.

---

## Development vs Production

### Development Mode

In development mode (DEV_MODE=true), the services use mock responses instead of calling the wrappers. This allows testing on x86 platforms without ARM64 native libraries.

### Production Mode

In production mode (DEV_MODE=false), the services load and use these wrappers to call the native engines.

---

## Troubleshooting

### Library Not Found

```
Error: /usr/lib/libwhisperwrapper.so not found
```

**Solution**: Ensure native libraries are copied to `/usr/lib/`:
```bash
# Check if libraries exist
ls -la /usr/lib/lib*wrapper.so

# Check LD_LIBRARY_PATH
echo $LD_LIBRARY_PATH
```

### Symbol Not Found

```
OSError: undefined symbol: whisper_create
```

**Solution**: Check library dependencies:
```bash
# Check library dependencies
ldd /usr/lib/libwhisperwrapper.so

# Ensure all dependencies are in /usr/lib/
ls -la /usr/lib/libwhisper*.so
```

### Callback Errors

```
TypeError: callback function must be callable
```

**Solution**: Ensure callback matches C signature:
```python
from ctypes import CFUNCTYPE, c_void_p, c_char_p

# Correct callback type
@CFUNCTYPE(None, c_void_p, c_char_p)
def my_callback(user_data, result):
    pass
```

### Segmentation Fault

```
Segmentation fault (core dumped)
```

**Solution**: Common causes:
- Incorrect ctypes function signatures
- Accessing freed memory
- Null pointer dereference
- Not calling `close()` or `deinit()`

Always ensure:
1. Function signatures match C API
2. Resources are properly cleaned up
3. Callbacks are kept alive during C calls

---

## Testing

### Test Scripts

Test scripts are available in `tests/` subdirectories:

```bash
# Test ASR wrapper
python tests/asr/server.py

# Test Translation wrapper
python tests/t2t/test_translation_wrapper.py

# Test TTS wrapper
python tests/tts/tts.py
```

### Manual Testing

```python
# Test library loading
from asr_wrapper import WhisperWrapper
wrapper = WhisperWrapper()
print("ASR wrapper loaded successfully")

from translate_wrapper import TranslationWrapper
print("Translation wrapper loaded successfully")

from tts_wrapper import TTS
tts = TTS()
print("TTS wrapper loaded successfully")
```

---

## Best Practices

### 1. Always Clean Up Resources

```python
wrapper = WhisperWrapper(...)
try:
    # Use wrapper
    pass
finally:
    wrapper.close()
```

### 2. Keep Callbacks Alive

```python
# Store callback as instance variable
self._callback = my_callback_function
wrapper.set_callback(self._callback)
```

### 3. Use Bytes for Strings

```python
# Correct
wrapper.init(model_path=b"/path/to/model")

# Incorrect
wrapper.init(model_path="/path/to/model")  # Will fail
```

### 4. Check Return Codes

```python
result = wrapper.process(text)
if result != 0:
    print(f"Processing failed with code {result}")
```

### 5. Handle Errors Gracefully

```python
try:
    wrapper.process(text)
except Exception as e:
    logger.error(f"Processing error: {e}", exc_info=True)
    # Continue or retry
```

---

## Future Enhancements

- [ ] Add context manager support (`__enter__`, `__exit__`)
- [ ] Add async/await support
- [ ] Add type hints throughout
- [ ] Add comprehensive unit tests
- [ ] Add performance profiling
- [ ] Add memory leak detection
- [ ] Add better error messages
- [ ] Add logging integration

---

## License

Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.  
SPDX-License-Identifier: BSD-3-Clause-Clear

---

## License

Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.  
SPDX-License-Identifier: BSD-3-Clause-Clear
