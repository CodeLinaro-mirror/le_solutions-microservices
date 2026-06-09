# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from abc import ABC, abstractmethod
import numpy as np


class ImageGeneratorInterface(ABC):
    """
    Abstract interface for image generation models.

    All image generation backends (e.g. Stable Diffusion 1.5, 2.1) must
    implement this interface so that callers are decoupled from the concrete
    implementation.
    """

    @abstractmethod
    def generate_image(
        self,
        prompt: str,
        seed: int = 0,
        steps: int = 20,
        guidance_scale: float = 7.5,
    ) -> np.ndarray:
        """
        Generate an image from a text prompt.

        Parameters
        ----------
        prompt : str
            Text description of the image to generate.
        seed : int
            Random seed for reproducibility.
        steps : int
            Number of diffusion denoising steps.
        guidance_scale : float
            Classifier-free guidance scale. Higher values steer the output
            more strongly toward the prompt.

        Returns
        -------
        np.ndarray
            Generated image with shape (1, H, W, 3), dtype float32,
            values in [0, 1].
        """
        pass

    @abstractmethod
    def get_model_id(self) -> str:
        """
        Return the model identifier string (e.g. 'stable-diffusion-2.1').
        """
        pass
