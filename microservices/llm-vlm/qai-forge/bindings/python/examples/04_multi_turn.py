# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# 04_multi_turn.py — Multi-turn conversation via the `user` session scope
#
# Demonstrates sending the complete transcript on every turn while scoping
# optional Genie runtime memory to a session id via `user=`.
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python 04_multi_turn.py [model_id]
# ─────────────────────────────────────────────────────────────────────────────

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from _common import resolve_model  # noqa: E402
from qai_forge import QaiForge, QaiForgeError  # noqa: E402

MODEL = resolve_model()
SESSION = "py-example-session-001"


def main() -> int:
    llm = QaiForge()
    print(f"Model: {MODEL} (override with argv[1] or QAI_FORGE_MODEL env var)\n")

    llm.start()
    llm.scan_models()
    try:
        messages = [{"role": "user", "content": "My name is Alice and I love Python."}]
        r1 = llm.generate(MODEL, messages, user=SESSION)
        print(f"Turn 1: {r1['content']}")

        messages.extend([
            {"role": "assistant", "content": r1["content"]},
            {"role": "user", "content": "What is my name and what do I love?"},
        ])
        r2 = llm.generate(MODEL, messages, user=SESSION)
        print(f"Turn 2: {r2['content']}")
        return 0
    except QaiForgeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        llm.release_conversation(SESSION)
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
