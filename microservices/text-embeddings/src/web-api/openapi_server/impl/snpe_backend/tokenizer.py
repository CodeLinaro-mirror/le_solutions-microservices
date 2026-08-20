# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import importlib.util

class TokenizerAdapter:
    def encode(self, text: str) -> list[int]:
        raise NotImplementedError


class HFTokenizerJSON(TokenizerAdapter):
    def __init__(self, json_path: str):
        try:
            from tokenizers import Tokenizer
        except Exception as e:
            raise RuntimeError(f"tokenizers not installed: {e}")

        self.tok = Tokenizer.from_file(json_path)

    def encode(self, text: str) -> list[int]:
        enc = self.tok.encode(text)
        return list(enc.ids)


class CustomTokenizer(TokenizerAdapter):
    def __init__(self, module_path: str, func_name: str):
        module_path = os.path.abspath(module_path)
        spec = importlib.util.spec_from_file_location("_custom_snpe_tok", module_path)
        if not spec or not spec.loader:
            raise RuntimeError(f"Cannot import custom SNPE tokenizer module: {module_path}")

        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        fn = getattr(mod, func_name, None)
        if not fn or not callable(fn):
            raise RuntimeError(f"Function '{func_name}' not found in {module_path}")

        self.fn = fn

    def encode(self, text: str) -> list[int]:
        out = self.fn(text)
        if not isinstance(out, (list, tuple)):
            raise RuntimeError("Custom SNPE tokenizer must return list[int]")
        return [int(x) for x in out]


def load_snpe_tokenizer(model_path: Optional[str] = None) -> TokenizerAdapter:
    # 1. Try to find dynamically near the model_path if provided
    if model_path:
        model_dir = os.path.dirname(model_path)
        if os.path.isdir(model_dir):
            json_path = os.path.join(model_dir, "tokenizer.json")
            if os.path.exists(json_path):
                return HFTokenizerJSON(json_path)
            # Recursively walk to find it
            try:
                for root, dirs, files in os.walk(model_dir):
                    depth = root[len(model_dir):].count(os.sep)
                    if depth > 2:
                        dirs.clear()
                        continue
                    if "tokenizer.json" in files:
                        found_json = os.path.join(root, "tokenizer.json")
                        return HFTokenizerJSON(found_json)
            except Exception:
                pass

    # 2. Try to find dynamically in the models root directory
    models_dir = os.getenv("T2E_MODEL_DIR", "/mnt/work/models")
    if os.path.isdir(models_dir):
        try:
            for root, dirs, files in os.walk(models_dir):
                depth = root[len(models_dir):].count(os.sep)
                if depth > 3:
                    dirs.clear()
                    continue
                if "tokenizer.json" in files:
                    found_json = os.path.join(root, "tokenizer.json")
                    return HFTokenizerJSON(found_json)
        except Exception:
            pass

    # 3. Fallback to TOKENIZER_DIR
    tok_dir = os.getenv("TOKENIZER_DIR")
    if not tok_dir:
        raise RuntimeError("TOKENIZER_DIR is not set")

    if not os.path.isdir(tok_dir):
        raise RuntimeError(f"SNPE Tokenizer directory does not exist: {tok_dir}")

    # 1. HF tokenizer.json
    json_path = os.path.join(tok_dir, "tokenizer.json")
    if os.path.exists(json_path):
        return HFTokenizerJSON(json_path)

    # 2. Custom tokenizer
    for fn in os.listdir(tok_dir):
        if fn.endswith(".py"):
            module_path = os.path.join(tok_dir, fn)
            return CustomTokenizer(module_path, "encode")

    raise RuntimeError(
        f"No tokenizer found in {tok_dir}. "
        "Expected tokenizer.json (HuggingFace format) or a custom .py with an encode() function."
    )
