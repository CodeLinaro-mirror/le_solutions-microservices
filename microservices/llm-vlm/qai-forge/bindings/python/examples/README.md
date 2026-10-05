# qai-forge Python Binding Examples

Each script is a standalone, runnable example demonstrating one feature of
the Python bindings (`qai_forge.py`).

## Setup

```bash
export QAI_FORGE_LIB=/path/to/libqai_forge.so
# or add its directory to LD_LIBRARY_PATH
```

## Model selection

Every example resolves the model id in this order:

1. `argv[1]` — e.g. `python 01_simple_chat.py Qwen3-1.7B`
2. `QAI_FORGE_MODEL` environment variable
3. Fallback default (`Qwen3-1.7B`, see `_common.py`)

## Examples

| Script                      | Demonstrates                                             |
|------------------------------|-----------------------------------------------------------|
| `01_simple_chat.py`          | Blocking chat completion                                  |
| `02_streaming_chat.py`       | Streaming chat completion with token-by-token output      |
| `03_reasoning.py`            | Reasoning models (`reasoning_content` / thinking tokens)  |
| `04_multi_turn.py`           | Multi-turn conversation via the `user` session scope      |
| `05_sampling_params.py`      | Custom temperature / top_p / top_k / max_tokens            |

## Run

```bash
python 01_simple_chat.py
python 02_streaming_chat.py Qwen3-1.7B
QAI_FORGE_MODEL=qwen2.5-7b python 04_multi_turn.py
```

## Lifecycle

Every example calls `QaiForge.start()` once before the first
`generate()`/`generate_stream()` call, and `QaiForge.shutdown()` when done.
This is required — the C API's internal scheduler must be started before
any inference request.

Every example also calls `QaiForge.scan_models()` immediately after
`start()`. This scans the models directory (`GENAI_MODELS_DIR` env var, or
the built-in default) and populates the model registry. **This is required**
— without it, every `generate()`/`generate_stream()` call fails with a 404
"Model not found" error, since the registry starts out empty.
