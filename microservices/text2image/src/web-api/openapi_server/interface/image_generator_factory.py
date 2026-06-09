# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import threading
from typing import Dict, Optional

from openapi_server.interface.image_generator import ImageGeneratorInterface
from openapi_server.interface.stable_diffusion_executor import (
    StableDiffusionV1_5Executor,
    StableDiffusionV2_1Executor,
)


class ImageGeneratorFactory:
    """
    Thread-safe singleton factory that creates and caches
    :class:`~openapi_server.interface.image_generator.ImageGeneratorInterface`
    instances keyed by ``model_id``.

    Because initialising an executor is expensive (it loads QNN context
    binaries and starts the QNN runtime), the factory reuses the same
    instance for all requests that target the same ``model_id``.  A new
    instance is only created the first time a particular ``model_id`` is
    requested.

    Usage
    -----
    .. code-block:: python

        from openapi_server.interface.image_generator_factory import (
            image_generator_factory,
        )

        executor = image_generator_factory.get_or_create(
            model_id="stable-diffusion-2.1",
            variant="2.1",
            config={
                "models_path": "/mnt/work/models",
                "text_encoder_model": "text_encoder.bin",
                "unet_model": "unet.bin",
                "vae_model": "vae.bin",
            },
        )
        image = executor.generate_image(prompt="a cat", seed=42, steps=20)
    """

    # Maps the variant string extracted from the model ID to the concrete
    # executor class.  Add new variants here without touching any other code.
    _MODEL_CLASS_MAP: Dict[str, type] = {
        "1.5": StableDiffusionV1_5Executor,
        "2.1": StableDiffusionV2_1Executor,
    }

    # Singleton bookkeeping
    _singleton_instance: Optional["ImageGeneratorFactory"] = None
    _singleton_lock: threading.Lock = threading.Lock()

    def __new__(cls) -> "ImageGeneratorFactory":
        if cls._singleton_instance is None:
            with cls._singleton_lock:
                if cls._singleton_instance is None:
                    instance = super().__new__(cls)
                    instance._registry: Dict[str, ImageGeneratorInterface] = {}
                    instance._registry_lock = threading.Lock()
                    cls._singleton_instance = instance
        return cls._singleton_instance

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def get_or_create(
        self,
        model_id: str,
        variant: str,
        config: dict,
    ) -> ImageGeneratorInterface:
        """
        Return the cached executor for *model_id*, creating it if necessary.

        Parameters
        ----------
        model_id : str
            Unique identifier for the model (e.g. ``"stable-diffusion-2.1"``).
            Used as the cache key.
        variant : str
            Variant string used to select the concrete executor class
            (e.g. ``"1.5"`` or ``"2.1"``).  Must be a key in
            :attr:`_MODEL_CLASS_MAP`.
        config : dict
            Infrastructure configuration forwarded to the executor constructor
            on first creation.  Must contain ``models_path``,
            ``text_encoder_model``, ``unet_model``, and ``vae_model``.

        Returns
        -------
        ImageGeneratorInterface
            A ready-to-use executor instance.

        Raises
        ------
        ValueError
            If *variant* is not supported.
        """
        with self._registry_lock:
            if model_id not in self._registry:
                executor_cls = self._MODEL_CLASS_MAP.get(variant)
                if executor_cls is None:
                    supported = list(self._MODEL_CLASS_MAP.keys())
                    raise ValueError(
                        f"Unsupported Stable Diffusion variant '{variant}'. "
                        f"Supported variants: {supported}"
                    )
                self._registry[model_id] = executor_cls(config)

            return self._registry[model_id]

    def invalidate(self, model_id: str) -> None:
        """
        Remove the cached executor for *model_id* and stop its QNN app.

        Call this when the model files on disk have changed and the executor
        needs to be re-initialised on the next request.

        Parameters
        ----------
        model_id : str
            The model identifier to evict from the cache.
        """
        with self._registry_lock:
            executor = self._registry.pop(model_id, None)
            if executor is not None:
                try:
                    executor.app.stop()
                except Exception:
                    pass

    def invalidate_all(self) -> None:
        """
        Remove all cached executors and stop their QNN apps.

        Useful during graceful shutdown or when the entire model directory
        has been replaced.
        """
        with self._registry_lock:
            for executor in self._registry.values():
                try:
                    executor.app.stop()
                except Exception:
                    pass
            self._registry.clear()

    def get_cached_model_ids(self) -> list:
        """
        Return the list of model IDs that are currently cached.

        Returns
        -------
        list[str]
        """
        with self._registry_lock:
            return list(self._registry.keys())


# Module-level singleton – import and use this directly.
image_generator_factory = ImageGeneratorFactory()
