# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# 01_simple_chat.py — Simple blocking chat completion
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python 01_simple_chat.py [model_id]
#
# Model selection (checked in order):
#   1. argv[1]
#   2. QAI_FORGE_MODEL env var
#   3. Fallback default (see _common.DEFAULT_MODEL)
# ─────────────────────────────────────────────────────────────────────────────

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from _common import resolve_model  # noqa: E402
from qai_forge import QaiForge, QaiForgeError  # noqa: E402

MODEL = resolve_model()


def main() -> int:
    llm = QaiForge()
    print(f"qai-forge version: {QaiForge.version()}")
    print(f"Model: {MODEL} (override with argv[1] or QAI_FORGE_MODEL env var)\n")

    llm.start()
    llm.scan_models()
    try:
        resp = llm.generate(MODEL, [{"role": "user", "content": "What is 2 + 2?"}])
        print(f"Answer: {resp['content']}")
        print(f"Tokens: {resp['prompt_tokens']} prompt / {resp['completion_tokens']} completion")
        return 0
    except QaiForgeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
