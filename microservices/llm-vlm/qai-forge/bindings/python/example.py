# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# example.py — Python bindings usage examples
#
# Run:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so
#   python example.py
# ─────────────────────────────────────────────────────────────────────────────

from qai_forge import QaiForge, QaiForgeError

llm = QaiForge()
print(f"qai-forge version: {QaiForge.version()}")

MODEL = "Qwen3-1.7B"

# ── Example 1: Blocking completion ────────────────────────────────────────────
print("\n=== Blocking completion ===")
try:
    resp = llm.chat(MODEL, [{"role": "user", "content": "What is 2 + 2?"}])
    print(f"Answer: {resp['content']}")
    print(f"Tokens: {resp['prompt_tokens']} prompt / {resp['completion_tokens']} completion")
except QaiForgeError as e:
    print(f"Error: {e}")

# ── Example 2: Streaming completion ───────────────────────────────────────────
print("\n=== Streaming completion ===")
print("Response: ", end="", flush=True)
try:
    for chunk in llm.chat_stream(MODEL, [{"role": "user", "content": "Tell me a short joke."}]):
        delta = chunk.get("content_delta", "")
        if delta:
            print(delta, end="", flush=True)
        if chunk.get("finish_reason"):
            print(f"\n[finish_reason: {chunk['finish_reason']}]")
except QaiForgeError as e:
    print(f"\nStream error: {e}")

# ── Example 3: Reasoning model (thinking tokens) ──────────────────────────────
print("\n=== Reasoning model (Qwen3 thinking) ===")
try:
    resp = llm.chat(
        MODEL,
        [{"role": "user", "content": "What is the square root of 144? Think step by step."}],
        max_tokens=512,
    )
    if resp.get("reasoning_content"):
        print(f"<think>\n{resp['reasoning_content']}\n</think>")
    print(f"Answer: {resp['content']}")
except QaiForgeError as e:
    print(f"Error: {e}")

# ── Example 4: Multi-turn conversation ────────────────────────────────────────
print("\n=== Multi-turn conversation ===")
SESSION = "example-session-001"
try:
    r1 = llm.chat(
        MODEL,
        [{"role": "user", "content": "My name is Alice and I love Python."}],
        user=SESSION,
    )
    print(f"Turn 1: {r1['content']}")

    r2 = llm.chat(
        MODEL,
        [{"role": "user", "content": "What is my name and what do I love?"}],
        user=SESSION,
    )
    print(f"Turn 2: {r2['content']}")
except QaiForgeError as e:
    print(f"Error: {e}")

# ── Example 5: Sampling parameters ────────────────────────────────────────────
print("\n=== Custom sampling parameters ===")
try:
    resp = llm.chat(
        MODEL,
        [{"role": "user", "content": "Write a haiku about the ocean."}],
        max_tokens=64,
        temperature=0.7,
        top_p=0.9,
        top_k=50,
    )
    print(resp["content"])
except QaiForgeError as e:
    print(f"Error: {e}")
