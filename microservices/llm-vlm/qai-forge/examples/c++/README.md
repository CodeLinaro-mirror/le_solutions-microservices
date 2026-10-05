# qai-forge C++ Examples

These examples demonstrate the `qai_forge::QaiForge` facade for on-device
generative/VLM inference via the GenIE, llama.cpp, and LiteRT-LM backends.

## Prerequisites

Download and set up the Qualcomm AI Runtime (QAIRT) SDK on-device before
building:

```bash
wget https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/2.48.40.260702/v2.48.40.260702.zip
unzip v2.48.40.260702.zip
cd 2.48.40.260702/

cd bin/
source ./envsetup.sh
echo $QAIRT_SDK_ROOT
sudo ${QAIRT_SDK_ROOT}/bin/check-linux-dependency.sh
grep QNN_SDK_BUILD_ID ${QAIRT_SDK_ROOT}/include/QNN/QnnSdkBuildId.h
```

`envsetup.sh` exports `QAIRT_SDK_ROOT`, which is required by the build
command below (`GENIE_INCLUDE_PATH`/`GENIE_LIB_PATH`).
`check-linux-dependency.sh` verifies the on-device package set the GenIE
backend needs is installed. The final `grep` line is just a sanity check to
confirm the SDK version you unpacked.

## Examples

| Directory | Demonstrates |
|---|---|
| `01_simple_chat/` | Minimal blocking `generate()` call |
| `02_streaming_chat/` | Token-by-token streaming via `generateStream()` |
| `03_multi_turn/` | Multi-turn conversation using `session_id`/`previous_response_id` |
| `04_reasoning/` | Reasoning/"thinking" models (`<think>` tag handling) |
| `05_vlm/` | Vision-language model inference (image + text input) |
| `06_tool_calling/` | Tool/function calling |

## Build

From the `qai-forge` root (after `source envsetup.sh` from the Prerequisites
step above has been run in the current shell):

```bash
cd examples/c++
mkdir build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DGENIE_INCLUDE_PATH=$QAIRT_SDK_ROOT/include/Genie \
  -DGENIE_LIB_PATH=$QAIRT_SDK_ROOT/lib/aarch64-oe-linux-gcc11.2 \
  -DQAI_FORGE_BUILD_GENIE=ON \
  -DQAI_FORGE_BUILD_WORKERS=ON
cmake --build . -j$(nproc)
```

Adjust `GENIE_LIB_PATH`'s architecture/toolchain suffix
(`aarch64-oe-linux-gcc11.2`) to match the lib directory actually present
under `$QAIRT_SDK_ROOT/lib/` on your board.

Each example produces its own binary, e.g. `build/01_simple_chat/01_simple_chat`.

## Running on real hardware

All examples select the model via `argv[1]` or the `QAI_FORGE_MODEL`
environment variable, falling back to a built-in default if neither is set:

```bash
./01_simple_chat <model_id>
# or
QAI_FORGE_MODEL=<model_id> ./01_simple_chat
```

Run any example with an invalid model id (e.g. `./01_simple_chat badmodelid`)
to print the list of available models discovered from `GENAI_MODELS_DIR`
(default `/mnt/work/models`) — this is the easiest way to find a valid
`<model_id>` for your device.

### Required environment variables for real hardware / non-default installs

On real Qualcomm hardware, or if you installed qai-forge to a non-default
`CMAKE_INSTALL_PREFIX` (e.g. `/usr/local` instead of `/usr`), set the
following before running any example:

```bash
export LOG_LEVEL=error
export QAI_FORGE_PLATFORM=ventunoq   # or: iq9
export GENAI_MODELS_DIR=<path to models>
export QAI_FORGE_PLUGIN_DIR=/usr/local/lib/qai-forge/plugins

QAI_FORGE_MODEL=<model_id> ./02_streaming_chat
```

- **`LOG_LEVEL`** — controls the GenIE SDK's own native logger (separate
  from qai-forge's internal logging). By default GenIE logs at `INFO`,
  which emits a `QnnGraph_execute started/done` + `Graph ... execution
  finished` line pair **per QNN sub-graph, per generated token** — this
  floods stdout on real hardware. Set `LOG_LEVEL=error` (or `warn`) to
  silence this. Valid values: `error`/`err`, `warn`/`warning`,
  `verbose`/`debug`; default is `info` if unset.

- **`QAI_FORGE_PLATFORM`** — selects a built-in hardware topology so the
  GenIE backend can resolve a physical HTP `device_id` for model placement.
  Without this (or `QNN_BACKEND_LIB`), the GenIE backend will fail to load
  any model with:
  ```
  GenIEBackend: placement plan must resolve to a physical device_id target
  ```
  Supported values: `ventunoq` (single-HTP boards), `iq9` (dual-HTP
  IQ9-QCS9075 boards). Alternatively, set `QNN_BACKEND_LIB=<path to
  libQnnHtp.so>` to have the topology auto-probed at startup instead of
  using a hardcoded preset.

- **`GENAI_MODELS_DIR`** — root directory scanned for model bundles
  (metadata.json/geniex.json/hf_manifest.json/bare .litertlm/.gguf).
  Defaults to `/mnt/work/models` if unset.

- **`QAI_FORGE_PLUGIN_DIR`** — directory containing the built
  `libqaiforge_plugin_*.so` files. Defaults to `/usr/lib/qai-forge/plugins`,
  which won't match a custom install prefix. Example:
  ```bash
  export QAI_FORGE_PLUGIN_DIR=/usr/local/lib/qai-forge/plugins
  ```

### Other useful environment variables

- `GENAI_CONTEXT_CAPPING` — cap every model's context window to N tokens (`-1` = no cap)
