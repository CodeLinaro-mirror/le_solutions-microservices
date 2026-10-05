# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# _common.py — Shared helpers for the Python binding examples
#
# Not a public module — imported by the example scripts in this directory
# only (via `from _common import resolve_model`).
# ─────────────────────────────────────────────────────────────────────────────

import os
import sys

# Fallback default used by every example when neither a CLI argument nor the
# QAI_FORGE_MODEL environment variable is set.
DEFAULT_MODEL = "Qwen3-1.7B"


def resolve_model(default: str = DEFAULT_MODEL) -> str:
    """
    Resolve the model id to use for an example run.

    Resolution order:
      1. sys.argv[1]           e.g. `python 01_simple_chat.py Qwen3-1.7B`
      2. QAI_FORGE_MODEL env var
      3. The given fallback default
    """
    if len(sys.argv) > 1 and sys.argv[1]:
        return sys.argv[1]
    env = os.environ.get("QAI_FORGE_MODEL")
    if env:
        return env
    return default
