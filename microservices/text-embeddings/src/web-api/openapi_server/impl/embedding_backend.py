# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Common abstract interface for all embedding inference backends.

Both `NomicEmbedBackend` (LiteRT / TFLite path, .tflite models) and
`SimpleQnnEmbeddingApp` (QNN binary path, .bin models) implement this
interface so that `EmbeddingsApiImpl._get_embeddings_from_component` can
work with either backend without any post-selection branching.

Implementing class contract
---------------------------
* ``embed_texts(texts)``  — run inference and return one float vector per text.
* ``encode_tokens(text)`` — tokenise a single string and return token IDs
                            (used only for usage/billing token counting).
* ``close()``             — release all native handles, DMA buffers, and
                            shared-library state.  Must be idempotent.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import List


class EmbeddingBackend(ABC):
    """
    Minimal interface shared by every embedding inference backend.

    Subclasses must be safe to call from a single thread at a time.
    The caller (``_get_backend`` in ``embeddings_api_impl``) is responsible
    for serialising concurrent requests with a per-instance lock.
    """

    # ------------------------------------------------------------------
    # Required interface
    # ------------------------------------------------------------------

    @abstractmethod
    def embed_texts(self, texts: List[str]) -> List[List[float]]:
        """
        Embed *texts* and return a list of float32 vectors.

        Parameters
        ----------
        texts:
            One or more input strings.

        Returns
        -------
        List[List[float]]
            One vector per input string, in the same order.
            Each inner list has length equal to the model's embedding
            dimension.
        """

    @abstractmethod
    def encode_tokens(self, text: str) -> List[int]:
        """
        Tokenise *text* and return the raw token IDs.

        Used exclusively for computing ``prompt_tokens`` / ``total_tokens``
        in the API response.  The result does not need to match the exact
        token sequence fed to the model (e.g. padding is excluded).

        Parameters
        ----------
        text:
            A single input string.

        Returns
        -------
        List[int]
            Token IDs without padding.
        """

    @abstractmethod
    def close(self) -> None:
        """
        Release all resources held by this backend.

        Must be idempotent — calling ``close()`` more than once must not
        raise an exception.
        """

    # ------------------------------------------------------------------
    # Convenience helpers (non-abstract, may be overridden)
    # ------------------------------------------------------------------

    def count_tokens(self, texts: List[str]) -> List[int]:
        """
        Return the token count for each string in *texts*.

        Default implementation calls ``encode_tokens`` once per string.
        Subclasses may override with a batched implementation.
        """
        return [len(self.encode_tokens(t)) for t in texts]
