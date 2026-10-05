# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# 02_streaming_chat.py — Streaming chat completion
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python 02_streaming_chat.py [model_id]
# ─────────────────────────────────────────────────────────────────────────────

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from _common import resolve_model  # noqa: E402
from qai_forge import QaiForge, QaiForgeError  # noqa: E402

MODEL = resolve_model()


def main() -> int:
    llm = QaiForge()
    print(f"Model: {MODEL} (override with argv[1] or QAI_FORGE_MODEL env var)\n")

    llm.start()
    llm.scan_models()
    print("Response: ", end="", flush=True)
    try:
        for chunk in llm.generate_stream(
            MODEL, [{"role": "user", "content": "Tell me a short joke."}]
        ):
            delta = chunk.get("content_delta", "")
            if delta:
                print(delta, end="", flush=True)
            if chunk.get("finish_reason"):
                print(f"\n[finish_reason: {chunk['finish_reason']}]")
        return 0
    except QaiForgeError as e:
        print(f"\nStream error: {e}", file=sys.stderr)
        return 1
    finally:
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
