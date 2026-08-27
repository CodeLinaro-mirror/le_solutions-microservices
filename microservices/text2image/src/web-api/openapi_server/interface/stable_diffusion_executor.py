# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import shutil
from abc import abstractmethod

import numpy as np
import torch
from diffusers import DPMSolverMultistepScheduler
from tokenizers import Tokenizer

from openapi_server.interface.image_generator import ImageGeneratorInterface
from openapi_server.interface.qnn_runtime import AppOptions, QnnSampleApp
from PIL import Image


# ---------------------------------------------------------------------------
# Abstract base class
# ---------------------------------------------------------------------------

class StableDiffusionExecutor(ImageGeneratorInterface):
    """
    Abstract base class for Stable Diffusion executors backed by QNN models.

    Subclasses must implement:
      - ``hidden_size`` property  – text-embedding dimension (768 for SD 1.5,
        1024 for SD 2.1).
      - ``_create_scheduler()``   – return a configured
        ``DPMSolverMultistepScheduler`` with the correct ``prediction_type``
        for the variant.
      - ``get_model_id()``        – return the model identifier string.

    The constructor accepts only *infrastructure* configuration (model paths
    and file names).  Per-request parameters (prompt, seed, steps,
    guidance_scale) are passed to :meth:`generate_image` at call time, which
    allows a single executor instance to be reused across many requests.
    """

    # JSON schema for infrastructure config validation
    INFRA_CONFIG_SCHEMA = {
        "type": "object",
        "required": ["models_path", "text_encoder_model", "unet_model", "vae_model"],
        "properties": {
            "models_path": {
                "type": "string",
                "description": "Directory containing the QNN model context binaries",
            },
            "text_encoder_model": {
                "type": "string",
                "description": "Filename of the text-encoder QNN context binary",
            },
            "unet_model": {
                "type": "string",
                "description": "Filename of the UNet QNN context binary",
            },
            "vae_model": {
                "type": "string",
                "description": "Filename of the VAE decoder QNN context binary",
            },
        },
        "additionalProperties": False,
    }

    TOKENIZER_MAX_LENGTH = 77

    def __init__(self, config: dict) -> None:
        """
        Initialise the executor with infrastructure configuration.

        Parameters
        ----------
        config : dict
            Must contain ``models_path``, ``text_encoder_model``,
            ``unet_model``, and ``vae_model``.
        """
        import jsonschema

        try:
            jsonschema.validate(instance=config, schema=self.INFRA_CONFIG_SCHEMA)
        except jsonschema.exceptions.ValidationError as exc:
            raise ValueError(f"Executor configuration error: {exc.message}") from exc

        self.models_path = config["models_path"]
        self.text_encoder_model = config["text_encoder_model"]
        self.unet_model = config["unet_model"]
        self.vae_model = config["vae_model"]

        self._init_tokenizer()
        self.scheduler = self._create_scheduler()

        self.app_opts = AppOptions(
            retrieve_contexts=[
                os.path.join(self.models_path, self.text_encoder_model),
                os.path.join(self.models_path, self.unet_model),
                os.path.join(self.models_path, self.vae_model),
            ],
            backend_path="/usr/lib/libQnnHtp.so",
            system_library="/usr/lib/libQnnSystem.so",
            input_list_paths="",
            output_dir="/tmp/sd_inference",
            log_level=4,
            profiling_level="off",
        )

        self.app = QnnSampleApp(self.app_opts)
        self.app.start()

    def __del__(self) -> None:
        try:
            if hasattr(self, "app") and self.app:
                self.app.stop()
        except Exception:
            pass

    # ------------------------------------------------------------------
    # Abstract interface – subclasses must implement these
    # ------------------------------------------------------------------

    @property
    @abstractmethod
    def hidden_size(self) -> int:
        """
        Dimensionality of the text encoder output embeddings.

        - SD 1.5 (CLIP ViT-L/14):      768
        - SD 2.1 (OpenCLIP ViT-H/14): 1024
        """

    @abstractmethod
    def _create_scheduler(self) -> DPMSolverMultistepScheduler:
        """
        Return a freshly constructed scheduler configured for this variant.

        The key difference between variants is ``prediction_type``:
          - SD 1.5: ``"epsilon"``   (standard noise prediction)
          - SD 2.1: ``"v_prediction"`` (velocity prediction)
        """

    @abstractmethod
    def get_model_id(self) -> str:
        """Return the model identifier string, e.g. ``'stable-diffusion-2.1'``."""

    # ------------------------------------------------------------------
    # Tokenizer
    # ------------------------------------------------------------------

    def _init_tokenizer(self) -> None:
        """Initialise the CLIP tokenizer."""
        print("Loading tokenizer")
        self.tokenizer = Tokenizer.from_pretrained("openai/clip-vit-large-patch14")
        self.tokenizer.enable_truncation(self.TOKENIZER_MAX_LENGTH)
        self.tokenizer.enable_padding(
            pad_id=49407, length=self.TOKENIZER_MAX_LENGTH
        )

    def run_tokenizer(self, prompt: str) -> np.ndarray:
        """
        Tokenise *prompt* and return token IDs as a float32 array.

        Returns
        -------
        np.ndarray
            Shape ``(1, TOKENIZER_MAX_LENGTH)``, dtype ``float32``.
        """
        print("prompt:", prompt)
        token_ids = self.tokenizer.encode(prompt).ids
        token_ids = np.array(token_ids, dtype=np.float32).reshape(1, -1)
        print("token_ids shape:", token_ids.shape)
        return token_ids

    # ------------------------------------------------------------------
    # QNN inference helpers
    # ------------------------------------------------------------------

    def run_qnn_net_run(
        self,
        model_type: str,
        model_context: str,
        input_data_list: list,
    ) -> np.ndarray:
        """
        Execute a single QNN graph and return its raw float32 output.

        Parameters
        ----------
        model_type : str
            One of ``"textencoder"``, ``"unet"``, or ``"vae"``.
        model_context : str
            Path to the QNN context binary (used only for logging).
        input_data_list : list[np.ndarray]
            Ordered list of input tensors.

        Returns
        -------
        np.ndarray
            Flat float32 output array.
        """
        tmp_dirpath = "/tmp/sd_inference"
        os.makedirs(tmp_dirpath, exist_ok=True)

        # Write inputs to raw files
        input_list_text = ""
        for index, input_data in enumerate(input_data_list):
            raw_file_path = os.path.join(tmp_dirpath, f"input_{index}.raw")
            input_data.tofile(raw_file_path)
            input_list_text += raw_file_path + " "

        input_list_filepath = os.path.join(tmp_dirpath, "input_list.txt")
        with open(input_list_filepath, "w") as f:
            f.write(input_list_text.strip())

        self.app.opts.input_list_paths = input_list_filepath
        self.app.opts.output_dir = tmp_dirpath

        exit_code = self.app.execute_graphs(model_type)

        if exit_code != 0:
            self._cleanup_tmp(tmp_dirpath)
            raise RuntimeError(
                f"QnnSampleApp failed for '{model_type}' with exit code {exit_code}"
            )

        # Collect output
        output_name_map = {
            "textencoder": "text_embedding.raw",
            "unet": "output_latent.raw",
            "vae": "image.raw",
        }
        output_filename = output_name_map.get(model_type, "output.raw")
        output_file_path = os.path.join(tmp_dirpath, "Result_0", output_filename)
        output_data = np.fromfile(output_file_path, dtype=np.float32)

        self._cleanup_tmp(tmp_dirpath)
        return output_data

    @staticmethod
    def _cleanup_tmp(dirpath: str) -> None:
        """Remove all files and subdirectories inside *dirpath*."""
        try:
            for name in os.listdir(dirpath):
                path = os.path.join(dirpath, name)
                if os.path.isfile(path) or os.path.islink(path):
                    os.remove(path)
                elif os.path.isdir(path):
                    shutil.rmtree(path)
        except Exception:
            pass

    # ------------------------------------------------------------------
    # Model-specific inference wrappers
    # ------------------------------------------------------------------

    def run_text_encoder(self, input_data: np.ndarray) -> np.ndarray:
        """
        Run the text encoder and return embeddings shaped
        ``(1, TOKENIZER_MAX_LENGTH, hidden_size)``.
        """
        model_path = os.path.join(self.models_path, self.text_encoder_model)
        output_data = self.run_qnn_net_run("textencoder", model_path, [input_data])
        return output_data.reshape((1, self.TOKENIZER_MAX_LENGTH, self.hidden_size))

    def run_unet(
        self,
        timestep_data: np.ndarray,
        latent_data: np.ndarray,
        text_emb_data: np.ndarray,
    ) -> np.ndarray:
        """
        Run the UNet denoiser.

        Inputs are expected in NHWC format; output is returned as
        ``(1, 64, 64, 4)`` float32.
        """
        print(
            f"UNet inputs – timestep: {timestep_data.shape} {timestep_data.dtype}, "
            f"latent: {latent_data.shape} {latent_data.dtype}, "
            f"text_emb: {text_emb_data.shape} {text_emb_data.dtype}"
        )

        if latent_data.shape != (1, 64, 64, 4):
            print(f"Warning: unexpected latent shape {latent_data.shape}, reshaping")
            if latent_data.size == 1 * 64 * 64 * 4:
                latent_data = latent_data.reshape(1, 64, 64, 4)

        if text_emb_data.shape != (1, self.TOKENIZER_MAX_LENGTH, self.hidden_size):
            expected = (1, self.TOKENIZER_MAX_LENGTH, self.hidden_size)
            print(f"Warning: unexpected text_emb shape {text_emb_data.shape}, expected {expected}")
            if text_emb_data.size == 1 * self.TOKENIZER_MAX_LENGTH * self.hidden_size:
                text_emb_data = text_emb_data.reshape(expected)

        model_path = os.path.join(self.models_path, self.unet_model)
        output_data = self.run_qnn_net_run(
            "unet", model_path, [timestep_data, latent_data, text_emb_data]
        )
        return output_data.reshape((1, 64, 64, 4))

    def run_vae(self, latent_data: np.ndarray) -> np.ndarray:
        """
        Run the VAE decoder.

        Input is expected in NHWC format ``(1, 64, 64, 4)``; output is
        returned as ``(1, 512, 512, 3)`` float32.
        """
        if latent_data.shape != (1, 64, 64, 4):
            print(f"Warning: unexpected latent shape {latent_data.shape}, reshaping")
            if latent_data.size == 1 * 64 * 64 * 4:
                latent_data = latent_data.reshape(1, 64, 64, 4)

        model_path = os.path.join(self.models_path, self.vae_model)
        output_data = self.run_qnn_net_run("vae", model_path, [latent_data])
        return output_data.reshape((1, 512, 512, 3))

    # ------------------------------------------------------------------
    # Main pipeline  (implements ImageGeneratorInterface)
    # ------------------------------------------------------------------

    def generate_image(
        self,
        prompt: str,
        seed: int = 0,
        steps: int = 20,
        guidance_scale: float = 7.5,
    ) -> np.ndarray:
        """
        Run the full Stable Diffusion pipeline and return the generated image.

        Parameters
        ----------
        prompt : str
            Text description of the desired image.
        seed : int
            Random seed for the initial latent noise.
        steps : int
            Number of diffusion denoising steps.
        guidance_scale : float
            Classifier-free guidance scale.

        Returns
        -------
        np.ndarray
            Shape ``(1, 512, 512, 3)``, dtype ``float32``, values in ``[0, 1]``.
        """
        # Configure scheduler for this request's step count
        self.scheduler.set_timesteps(steps)

        # Tokenise
        print("Tokenising prompts…")
        uncond_tokens = self.run_tokenizer("")
        cond_tokens = self.run_tokenizer(prompt)

        # Text encoder
        print("Running text encoder…")
        uncond_text_embedding = self.run_text_encoder(uncond_tokens)
        user_text_embedding = self.run_text_encoder(cond_tokens)

        embedding_diff = np.abs(uncond_text_embedding - user_text_embedding).mean()
        print(f"Embedding difference (cond vs uncond): {embedding_diff:.4f}")
        if embedding_diff < 0.01:
            print(
                "WARNING: embeddings are very similar – text encoder may not be "
                "working correctly."
            )

        # Initial latent noise
        print(f"Initialising latents with seed {seed}")
        generator = torch.manual_seed(seed)
        latents = torch.randn((1, 4, 64, 64), generator=generator)
        latents = latents * self.scheduler.init_noise_sigma

        # Diffusion loop
        print(f"Starting diffusion loop ({steps} steps)…")
        for step, t in enumerate(self.scheduler.timesteps):
            print(f"Step {step + 1}/{steps}  (t={t})")

            timestep_input = np.array([[t.numpy()]], dtype=np.float32)
            latent_model_input = self.scheduler.scale_model_input(latents, t)

            # NCHW → NHWC for QNN
            latent_in = latent_model_input.numpy().transpose((0, 2, 3, 1)).copy()

            # Unconditional pass
            noise_uncond = self.run_unet(timestep_input, latent_in, uncond_text_embedding)
            # Conditional pass
            noise_cond = self.run_unet(timestep_input, latent_in, user_text_embedding)

            # NHWC → NCHW for PyTorch scheduler
            noise_uncond_nchw = torch.from_numpy(noise_uncond.transpose((0, 3, 1, 2)))
            noise_cond_nchw = torch.from_numpy(noise_cond.transpose((0, 3, 1, 2)))

            noise_diff = torch.abs(noise_cond_nchw - noise_uncond_nchw).mean().item()
            print(f"  Noise prediction difference: {noise_diff:.4f}")
            if noise_diff < 0.001:
                print("  WARNING: noise predictions are very similar.")

            # Classifier-free guidance
            noise_pred = noise_uncond_nchw + guidance_scale * (
                noise_cond_nchw - noise_uncond_nchw
            )

            if torch.isnan(noise_pred).any() or torch.isinf(noise_pred).any():
                print("  WARNING: NaN/Inf in noise prediction – replacing with zeros.")
                noise_pred = torch.nan_to_num(noise_pred)

            latents = self.scheduler.step(noise_pred, t, latents).prev_sample

            if torch.isnan(latents).any() or torch.isinf(latents).any():
                print("  WARNING: NaN/Inf in latents – replacing with zeros.")
                latents = torch.nan_to_num(latents)

        print("Diffusion complete. Running VAE decoder…")

        # The aihub-compiled VAE binary already applies both the scaling factor
        # (z / 0.18215) and the output normalisation ((image / 2 + 0.5).clamp(0, 1))
        # internally, so we pass the raw latents directly and only clip as a
        # safety guard against minor floating-point out-of-range values.
        latents_for_vae = latents.numpy().transpose((0, 2, 3, 1))  # NCHW → NHWC

        output_raw_image = self.run_vae(latents_for_vae)
        print(
            f"VAE output shape: {output_raw_image.shape}  "
            f"range: [{output_raw_image.min():.3f}, {output_raw_image.max():.3f}]"
        )

        # VAE output is already in [0, 1] — clip only as a safety guard
        output_raw_image = output_raw_image.clip(0.0, 1.0)
        return output_raw_image

    # ------------------------------------------------------------------
    # Utility
    # ------------------------------------------------------------------

    def save_image(self, raw_image: np.ndarray, filename: str | None = None) -> str:
        """
        Save *raw_image* (float32, [0, 1]) to a PNG file.

        Parameters
        ----------
        raw_image : np.ndarray
            Shape ``(1, 512, 512, 3)``, dtype ``float32``.
        filename : str, optional
            Output path.  Auto-generated from the model ID if not provided.

        Returns
        -------
        str
            Path of the saved file.
        """
        img_uint8 = (raw_image * 255.0).clip(0, 255).astype(np.uint8)[0]
        img_uint8 = np.ascontiguousarray(img_uint8)
        pil_img = Image.fromarray(img_uint8, mode="RGB")

        if filename is None:
            filename = f"{self.get_model_id().replace('.', '_')}_output.png"

        pil_img.save(filename)
        print(f"Saved {filename}")
        return filename


# ---------------------------------------------------------------------------
# Stable Diffusion 2.1
# ---------------------------------------------------------------------------

class StableDiffusionV2_1Executor(StableDiffusionExecutor):
    """
    Stable Diffusion 2.1 executor.

    Key characteristics (from the aihub models project):
      - Text encoder: OpenCLIP ViT-H/14  →  ``hidden_size = 1024``
      - Scheduler: ``prediction_type = "v_prediction"`` (velocity prediction)
    """

    MODEL_ID = "stable-diffusion-2.1"

    @property
    def hidden_size(self) -> int:
        return 1024

    def _create_scheduler(self) -> DPMSolverMultistepScheduler:
        """
        DPMSolver++ scheduler configured for SD 2.1's v-prediction objective.

        SD 2.1 was trained with the *v-prediction* parameterisation
        (Salimans & Ho, 2022), which predicts the velocity of the diffusion
        process rather than the noise.  Using ``prediction_type="epsilon"``
        here would produce severely distorted images.
        """
        return DPMSolverMultistepScheduler(
            num_train_timesteps=1000,
            beta_start=0.00085,
            beta_end=0.012,
            beta_schedule="scaled_linear",
            prediction_type="v_prediction",
            algorithm_type="dpmsolver++",
            thresholding=False,
            use_karras_sigmas=True,
        )

    def get_model_id(self) -> str:
        return self.MODEL_ID


# ---------------------------------------------------------------------------
# Stable Diffusion 1.5
# ---------------------------------------------------------------------------

class StableDiffusionV1_5Executor(StableDiffusionExecutor):
    """
    Stable Diffusion 1.5 executor.

    Key characteristics (from the aihub models project):
      - Text encoder: CLIP ViT-L/14  →  ``hidden_size = 768``
      - Scheduler: ``prediction_type = "epsilon"`` (standard noise prediction)
    """

    MODEL_ID = "stable-diffusion-1.5"

    @property
    def hidden_size(self) -> int:
        return 768

    def _create_scheduler(self) -> DPMSolverMultistepScheduler:
        """
        DPMSolver++ scheduler configured for SD 1.5's epsilon-prediction objective.

        SD 1.5 was trained with the standard *epsilon* (noise) prediction
        parameterisation.  The original checkpoint ships with a PNDMScheduler,
        but DPMSolver++ with ``prediction_type="epsilon"`` produces equivalent
        or better quality at fewer steps.
        """
        return DPMSolverMultistepScheduler(
            num_train_timesteps=1000,
            beta_start=0.00085,
            beta_end=0.012,
            beta_schedule="scaled_linear",
            prediction_type="epsilon",
            algorithm_type="dpmsolver++",
            thresholding=False,
            use_karras_sigmas=True,
        )

    def get_model_id(self) -> str:
        return self.MODEL_ID
