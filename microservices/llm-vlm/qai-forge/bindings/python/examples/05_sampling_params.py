# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# 05_sampling_params.py — Custom sampling parameters
#
# Demonstrates temperature / top_p / top_k / max_tokens.
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python 05_sampling_params.py [model_id]
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
            [{"role": "user", "content": "Write a haiku about the ocean."}],
            max_tokens=64,
            temperature=0.7,
            top_p=0.9,
            top_k=50,
        )
        print(resp["content"])
        return 0
    except QaiForgeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    finally:
        llm.shutdown()


if __name__ == "__main__":
    sys.exit(main())
