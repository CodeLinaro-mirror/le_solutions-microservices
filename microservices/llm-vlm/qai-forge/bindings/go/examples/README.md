# qai-forge Go Binding Examples

Each subdirectory is a standalone `main` package demonstrating one feature
of the Go bindings (`qaiforge` package).

## Build requirements

```bash
export CGO_CFLAGS="-I/path/to/qai-forge/include"
export CGO_LDFLAGS="-L/path/to/lib -lqai_forge"
```

This directory tree is a single Go module rooted at `bindings/go/go.mod`
(module name `qaiforge`). The `qaiforge` package lives at
`bindings/go/qaiforge/` and is imported by each example as
`qaiforge/qaiforge`. Running `go run main.go` from inside any
`examples/NN_*/` subdirectory will automatically resolve against the
module root — no additional `go.mod` is needed per example. If you add new
dependencies, run `go mod tidy` from `bindings/go/`.

## Model selection

Every example resolves the model id in this order:

1. `os.Args[1]` — e.g. `go run main.go Qwen3-1.7B`
2. `QAI_FORGE_MODEL` environment variable
3. Fallback default (`Qwen3-1.7B`)

## Examples

| Directory               | Demonstrates                                           |
|--------------------------|---------------------------------------------------------|
| `01_simple_chat/`         | Blocking chat completion                                |
| `02_streaming_chat/`      | Streaming chat completion with token-by-token output    |
| `03_reasoning/`           | Reasoning models (`ReasoningContent` / thinking tokens) |
| `04_multi_turn/`          | Multi-turn conversation via the `User` session scope    |
| `05_sampling_params/`     | Custom temperature / top_p / top_k / max tokens          |

## Run

```bash
cd 01_simple_chat && go run main.go
cd 02_streaming_chat && go run main.go Qwen3-1.7B
QAI_FORGE_MODEL=qwen2.5-7b go run 04_multi_turn/main.go
```

## Lifecycle

Every example calls `qaiforge.Start()` once before the first
`Generate`/`GenerateStream` call, and `qaiforge.Shutdown(false)` when done
(typically via `defer`). This is required — the C API's internal scheduler
must be started before any inference request.

Every example also calls `qaiforge.ScanModels()` after `Start()`. This
scans the models directory (`GENAI_MODELS_DIR` env var, or the built-in
default) and populates the model registry. **This is required** — without
it, every `Generate`/`GenerateStream` call fails with a 404 "Model not
found" error, since the registry starts out empty.
