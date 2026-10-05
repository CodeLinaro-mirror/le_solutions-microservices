# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# 03_reasoning.py — Reasoning model (thinking tokens)
#
# Demonstrates reasoning_content for models with supports_thinking=true
# (e.g. Qwen3, DeepSeek-R1).
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python 03_reasoning.py [model_id]
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
    try:
        resp = llm.generate(
            MODEL,
            [{"role": "user", "content": "What is the square root of 144? Think step by step."}],
            max_tokens=512,
        )
        if resp.get("reasoning_content"):
            print(f"<think>\n{resp['reasoning_content']}\n</think>")
        print(f"Answer: {resp['content']}")
        return 0
    except QaiForgeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
