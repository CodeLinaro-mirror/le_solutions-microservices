# qai-forge Rust Binding Examples

Each file is a standalone example binary demonstrating one feature of the
Rust bindings (`qai_forge` crate).

## Build requirements

```bash
export QAI_FORGE_LIB_DIR=/path/to/lib
```

## Model selection

Every example resolves the model id in this order:

1. The first CLI argument — e.g. `cargo run --example 01_simple_chat -- Qwen3-1.7B`
2. `QAI_FORGE_MODEL` environment variable
3. Fallback default (`Qwen3-1.7B`, see `common.rs`)

## Examples

| Binary                   | Demonstrates                                           |
|----------------------------|---------------------------------------------------------|
| `01_simple_chat`          | Blocking chat completion                                |
| `02_streaming_chat`       | Streaming chat completion with token-by-token output    |
| `03_reasoning`            | Reasoning models (`reasoning_content` / thinking tokens)|
| `04_multi_turn`           | Multi-turn conversation via the `user` session scope    |
| `05_sampling_params`      | Custom temperature / top_p / top_k / max tokens          |

## Run

```bash
cargo run --example 01_simple_chat
cargo run --example 02_streaming_chat -- Qwen3-1.7B
QAI_FORGE_MODEL=qwen2.5-7b cargo run --example 04_multi_turn
```

## Lifecycle

Every example calls `qai_forge::start()` once before the first
`generate`/`generate_stream` call, and `qai_forge::shutdown(false)` when
done. This is required — the C API's internal scheduler must be started
before any inference request.

Every example also calls `qai_forge::scan_models()` after `start()`. This
scans the models directory (`GENAI_MODELS_DIR` env var, or the built-in
default) and populates the model registry. **This is required** — without
it, every `generate`/`generate_stream` call fails with a 404 "Model not
found" error, since the registry starts out empty.

## Note on `common.rs`

`common.rs` is a shared helper (model-id resolution) included directly via
`include!("common.rs")` in each example binary, since Cargo's `[[example]]`
targets are independent binaries and cannot easily share code without a
separate library crate. It is excluded from Cargo's example auto-discovery
via `autoexamples = false` in `Cargo.toml`.

Because `common.rs` is spliced into another file partway through (after
that file's own `use` statements), it must use plain `//` comments and
outer `#[...]` attributes rather than inner `//!` doc comments or `#![...]`
attributes — the latter are only valid at the very start of a file compiled
as its own module, which `common.rs` is not. Using `//!`/`#![...]` here
produces a `E0753: expected outer doc comment` compile error.
