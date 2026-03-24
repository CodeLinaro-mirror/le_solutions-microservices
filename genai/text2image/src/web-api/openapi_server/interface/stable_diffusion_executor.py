# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import shutil
import json
import numpy as np
import subprocess
import torch
import jsonschema
from tokenizers import Tokenizer
from diffusers import DPMSolverMultistepScheduler
from openapi_server.interface.qnn_runtime import AppOptions, QnnSampleApp
from PIL import Image

class StableDiffusionExecutor:
    """
    A class to execute Stable Diffusion v2.1 image generation using QNN models.
    """

    # Define the JSON schema for configuration validation
    CONFIG_SCHEMA = {
        "type": "object",
        "properties": {
            "prompt": {
                "type": "string",
                "description": "Text prompt for image generation"
            },
            "seed": {
                "type": "integer",
                "minimum": 0,
                "description": "Random seed for generation"
            },
            "steps": {
                "type": "integer",
                "minimum": 1,
                "maximum": 100,
                "description": "Number of diffusion steps"
            },
            "guidance_scale": {
                "type": "number",
                "minimum": 1.0,
                "maximum": 20.0,
                "description": "Guidance scale for classifier-free guidance"
            },
            "models_path": {
                "type": "string",
                "description": "Path to the model context files"
            },
            "variant": {
                "type": "string",
                "enum": ["2.1"],
                "description": "Stable Diffusion variant"
            }
        },
        "additionalProperties": False
    }

    def __init__(self, config=None):
        """
        Initialize the Stable Diffusion executor.

        Args:
            config (dict or str): Configuration as a dictionary or JSON string/file path.
                                 If None, default configuration will be used.
        """
        # Default configuration
        default_config = {
            "prompt": "decorated modern country house interior, high resolution, light reflections",
            "seed": 0,
            "steps": 20,
            "guidance_scale": 7.5,
            "models_path": "/opt/image_gen",
            "variant": "2.1"
        }

        # Process the configuration
        if config is None:
            self.config = default_config
        elif isinstance(config, dict):
            # Merge with default config
            self.config = {**default_config, **config}
        elif isinstance(config, str):
            # Check if it's a file path or a JSON string
            if os.path.isfile(config):
                with open(config, 'r') as f:
                    config_dict = json.load(f)
            else:
                try:
                    config_dict = json.loads(config)
                except json.JSONDecodeError:
                    raise ValueError(f"Invalid JSON string: {config}")

            # Merge with default config
            self.config = {**default_config, **config_dict}
        else:
            raise TypeError("Config must be a dictionary, JSON string, file path, or None")

        # Validate the configuration against the schema
        try:
            jsonschema.validate(instance=self.config, schema=self.CONFIG_SCHEMA)
        except jsonschema.exceptions.ValidationError as e:
            raise ValueError(f"Configuration validation error: {e.message}")

        # Extract parameters from config
        self.prompt = self.config["prompt"]
        self.seed = np.int64(self.config["seed"])
        self.steps = self.config["steps"]
        self.guidance_scale = self.config["guidance_scale"]
        self.models_path = self.config["models_path"]
        self.variant = self.config["variant"]
        self.tokenizer_max_length = 77

        # Set up model configuration
        if self.variant == "2.1":
            self.hidden_size = 1024
            self.tokenizer_repo = "stabilityai/stable-diffusion-2-1-base"
            self.tokenizer_subfolder = 'tokenizer'
            self.tokenizer_revision = 'main'
        else:
            raise ValueError(f"Stable Diffusion variant must be '2.1', found {self.variant}")

        # Initialize components
        self._init_tokenizer()
        self._init_scheduler()

        # Print configuration
        print(f"Using prompt: '{self.prompt}'")
        print(f"Using seed: {self.seed}")
        print(f"Using steps: {self.steps}")
        print(f"Using guidance scale: {self.guidance_scale}")

        self.app_opts = AppOptions(
            retrieve_context=self.models_path,            # directory with the 3 .bin files
            backend_path="/usr/lib/libQnnHtp.so",
            system_library="/usr/lib/libQnnSystem.so",
            input_list_paths="",                          # will be set per run
            output_dir=os.path.join(os.getcwd(), "tmp"),  # will be overridden per run
            log_level=4,                                  # 0=ERROR,1=WARN,2=INFO,3=DEBUG,4=VERBOSE
            profiling_level="off",
        )

        # Start a single persistent app
        self.app = QnnSampleApp(self.app_opts)
        self.app.start()

    def __del__(self):
        # Ensure the QNN app is stopped when executor is destroyed
        try:
            if hasattr(self, "app") and self.app:
                self.app.stop()
        except Exception:
            pass

    def _init_tokenizer(self):
        """Initialize the tokenizer."""
        print("Loading tokenizer")
        self.tokenizer = Tokenizer.from_pretrained("openai/clip-vit-large-patch14")
        self.tokenizer.enable_truncation(self.tokenizer_max_length)
        self.tokenizer.enable_padding(pad_id=49407, length=self.tokenizer_max_length)

    def _init_scheduler(self):
        """Initialize the scheduler."""
        print("Initializing the Scheduler")
        self.scheduler = DPMSolverMultistepScheduler(
            num_train_timesteps=1000,
            beta_start=0.00085,
            beta_end=0.012,
            beta_schedule="scaled_linear"
        )
        self.scheduler.set_timesteps(self.steps)

    def run_tokenizer(self, prompt):
        """
        Run the tokenizer on the given prompt.

        Args:
            prompt (str): The text prompt to tokenize

        Returns:
            np.ndarray: Token IDs as float32 numpy array
        """
        print("prompt: ", prompt)
        token_ids = self.tokenizer.encode(prompt).ids
        # Convert tokens list to np.array
        token_ids = np.array(token_ids, dtype=np.float32).reshape(1, -1)
        print("token_ids type: ", type(token_ids), " shape: ", token_ids.shape)
        print("First few tokens: ", token_ids[0][:10])
        return token_ids

    def run_qnn_net_run(self, type, model_context, input_data_list):
        """
        Run a QNN model with the given inputs.

        Args:
            type (str): Model type ("textencoder", "unet", or "vae")
            model_context (str): Path to the model context file
            input_data_list (list): List of numpy arrays as inputs

        Returns:
            np.ndarray: Output data as float32 numpy array
        """
        # Define tmp directory path for intermediate artifacts
        tmp_dirpath = os.path.join(os.getcwd(), 'tmp')
        os.makedirs(tmp_dirpath, exist_ok=True)

        # Dump each input data from input_data_list as raw file
        input_list_text = ''
        for index, input_data in enumerate(input_data_list):
            raw_file_path = os.path.join(tmp_dirpath, f'input_{index}.raw')
            input_data.tofile(raw_file_path)
            input_list_text += raw_file_path + ' '

        # Create input_list_filepath and add prepared input_list_text into this file
        input_list_filepath = os.path.join(tmp_dirpath, 'input_list.txt')
        with open(input_list_filepath, 'w') as f:
            f.write(input_list_text.strip())

        # ---- Update dynamic AppOptions for this invocation (no restart) ----
        self.app.opts.input_list_paths = input_list_filepath
        self.app.opts.output_dir = tmp_dirpath

        # Execute only the manager corresponding to `type`
        # (Substring match on the manager's retrieve_context basename)
        exit_code = self.app.execute_graphs(type)

        if exit_code != 0:
            # Cleanup before raising, to keep behavior neat
            try:
                for filename in os.listdir(tmp_dirpath):
                    file_path = os.path.join(tmp_dirpath, filename)

                    if os.path.isfile(file_path) or os.path.islink(file_path):
                        os.remove(file_path)
                    elif os.path.isdir(file_path):
                        shutil.rmtree(file_path)

            except Exception:
                pass
            raise RuntimeError(f"QnnSampleApp failed with exit code {exit_code}")

        # Collect output
        if type == "textencoder":
            output_file_path = os.path.join(tmp_dirpath, 'Result_0', 'text_embedding.raw')
        elif type == "unet":
            output_file_path = os.path.join(tmp_dirpath, 'Result_0', 'output_latent.raw')
        else:
            output_file_path = os.path.join(tmp_dirpath, 'Result_0', 'image.raw')

        output_data = np.fromfile(output_file_path, dtype=np.float32)

        for filename in os.listdir(tmp_dirpath):
            file_path = os.path.join(tmp_dirpath, filename)
            if os.path.isfile(file_path) or os.path.islink(file_path):
                os.remove(file_path)
            elif os.path.isdir(file_path):
                shutil.rmtree(file_path)

        return output_data

    def run_text_encoder(self, input_data):
        """
        Run the text encoder model.

        Args:
            input_data (np.ndarray): Token IDs

        Returns:
            np.ndarray: Text embeddings
        """
        model_path = f'{self.models_path}/stable_diffusion_v2_1-text_encoder-qualcomm_sa8775p.bin'
        output_data = self.run_qnn_net_run("textencoder", model_path, [input_data])
        # Output of Text encoder should be of shape (1, 77, hidden_size)
        output_data = output_data.reshape((1, 77, self.hidden_size))
        return output_data

    def run_unet(self, timestep_data, latent_data, text_emb_data):
        """
        Run the UNet model.

        Args:
            timestep_data (np.ndarray): Timestep as 1x1 float32 array
            latent_data (np.ndarray): Latent data in NHWC format
            text_emb_data (np.ndarray): Text embeddings

        Returns:
            np.ndarray: Noise prediction
        """
        # Debug: Print input shapes and types
        print(f"UNet inputs - timestep: {timestep_data.shape} {timestep_data.dtype}, "
              f"latent: {latent_data.shape} {latent_data.dtype}, "
              f"text_emb: {text_emb_data.shape} {text_emb_data.dtype}")

        # Ensure latent is in NHWC format (1x64x64x4)
        if latent_data.shape != (1, 64, 64, 4):
            print(f"Warning: Unexpected latent shape: {latent_data.shape}, expected (1, 64, 64, 4)")
            if latent_data.size == 1 * 64 * 64 * 4:
                latent_data = latent_data.reshape(1, 64, 64, 4)

        # Ensure text embedding has correct shape
        if text_emb_data.shape != (1, 77, 1024):
            print(f"Warning: Unexpected text embedding shape: {text_emb_data.shape}, expected (1, 77, 1024)")
            if text_emb_data.size == 1 * 77 * 1024:
                text_emb_data = text_emb_data.reshape(1, 77, 1024)

        # Run the model
        model_path = f'{self.models_path}/stable_diffusion_v2_1-unet-qualcomm_sa8775p.bin'
        output_data = self.run_qnn_net_run("unet", model_path, [timestep_data, latent_data, text_emb_data])

        # Output of UNet should be of shape (1, 64, 64, 4)
        output_data = output_data.reshape((1, 64, 64, 4))
        return output_data

    def run_vae(self, latent_data):
        """
        Run the VAE decoder model.

        Args:
            latent_data (np.ndarray): Latent data in NHWC format

        Returns:
            np.ndarray: Decoded image
        """
        # Ensure latent is in NHWC format (1x64x64x4)
        if latent_data.shape != (1, 64, 64, 4):
            print(f"Warning: Unexpected latent shape: {latent_data.shape}, expected (1, 64, 64, 4)")
            if latent_data.size == 1 * 64 * 64 * 4:
                latent_data = latent_data.reshape(1, 64, 64, 4)

        # Run the model
        model_path = f'{self.models_path}/stable_diffusion_v2_1-vae-qualcomm_sa8775p.bin'
        output_data = self.run_qnn_net_run("vae", model_path, [latent_data])

        # Output of VAE should be of shape (1, 512, 512, 3)
        output_data = output_data.reshape((1, 512, 512, 3))
        return output_data

    def generate_image(self):
        """
        Generate an image using the Stable Diffusion pipeline.

        Returns:
            np.ndarray: Raw output image as a numpy array with shape (1, 512, 512, 3)
                        and dtype float32, with values typically in the range [0, 1]
        """
        # Run Tokenizer
        print("Tokenizing prompts...")
        uncond_tokens = self.run_tokenizer("")
        cond_tokens = self.run_tokenizer(self.prompt)

        # Run Text Encoder on Tokens
        print("Running text encoder...")
        uncond_text_embedding = self.run_text_encoder(uncond_tokens)
        user_text_embedding = self.run_text_encoder(cond_tokens)

        # Verify text embeddings are different
        embedding_diff = np.abs(uncond_text_embedding - user_text_embedding).mean()
        print(f"Average difference between conditional and unconditional embeddings: {embedding_diff}")
        if embedding_diff < 0.01:
            print("WARNING: Text embeddings are very similar! This may indicate a problem with the text encoder.")

        # Initialize the latent input with random initial latent
        print("Initializing latents with seed:", self.seed)
        generator = torch.manual_seed(self.seed)
        latents = torch.randn((1, 4, 64, 64), generator=generator)
        latents = latents * self.scheduler.init_noise_sigma

        print(f"Starting diffusion process with {self.steps} steps...")
        # Run the loop for user_step times
        for step, t in enumerate(self.scheduler.timesteps):
            print(f'Step {step+1}/{self.steps} Running... (t={t})')

            # Create timestep tensor with shape 1x1
            timestep_input = np.array([[t.numpy()]], dtype=np.float32)

            # Scale model input properly
            latent_model_input = self.scheduler.scale_model_input(latents, t)

            # Convert to NHWC format for the UNet (1x64x64x4)
            latent_in = latent_model_input.numpy().transpose((0, 2, 3, 1)).copy()

            # Run U-net for unconditional embeddings
            print("Running UNet for unconditional embedding...")
            noise_uncond = self.run_unet(timestep_input, latent_in, uncond_text_embedding)

            # Run U-net for conditional (text) embeddings
            print("Running UNet for conditional embedding...")
            noise_cond = self.run_unet(timestep_input, latent_in, user_text_embedding)

            # Convert noise predictions back to NCHW format for PyTorch processing
            noise_uncond_nchw = torch.from_numpy(noise_uncond.transpose((0, 3, 1, 2)))
            noise_cond_nchw = torch.from_numpy(noise_cond.transpose((0, 3, 1, 2)))

            # Check if noise predictions are different
            noise_diff = torch.abs(noise_cond_nchw - noise_uncond_nchw).mean().item()
            print(f"Average difference between conditional and unconditional noise: {noise_diff}")
            if noise_diff < 0.001:
                print("WARNING: Noise predictions are very similar! This may indicate a problem with the UNet.")

            # Perform classifier-free guidance
            noise_pred = noise_uncond_nchw + self.guidance_scale * (noise_cond_nchw - noise_uncond_nchw)

            # Check for numerical stability
            if torch.isnan(noise_pred).any() or torch.isinf(noise_pred).any():
                print("Warning: NaN or Inf values detected in noise prediction!")
                # Replace NaN/Inf values with zeros to prevent propagation
                noise_pred = torch.nan_to_num(noise_pred)

            # Step the scheduler
            latents = self.scheduler.step(noise_pred, t, latents).prev_sample

            # Check for numerical stability in latents
            if torch.isnan(latents).any() or torch.isinf(latents).any():
                print("Warning: NaN or Inf values detected in latents!")
                # Replace NaN/Inf values with zeros to prevent propagation
                latents = torch.nan_to_num(latents)

            print(f"Completed step {step+1}/{self.steps}")

        print("Diffusion process complete. Running VAE decoder...")
        # Convert final latents to NHWC format for VAE (1x64x64x4)
        latents_for_vae = latents.numpy().transpose((0, 2, 3, 1))

        # Run VAE decoder
        output_raw_image = self.run_vae(latents_for_vae)
        print("Output image shape:", output_raw_image.shape)
        return output_raw_image

    def save_image(self, raw_image, filename=None):
        """
        Save the generated image to a file.

        Args:
            raw_image (np.ndarray): Raw output image from generate_image() method,
                                    with shape (1, 512, 512, 3) and dtype float32
            filename (str, optional): The filename to save to. If None, a filename will be generated
                                    based on the prompt and seed.

        Returns:
            str: The filename the image was saved to
        """
        print("Raw image shape:", raw_image.shape)
        print("Raw image dtype:", raw_image.dtype)

        # Scale to [0,255] for display
        raw_image = (raw_image * 255.0).clip(0, 255).astype(np.uint8)

        print("Processed image shape:", raw_image.shape)
        print("Processed image dtype:", raw_image.dtype)

        # Extract the image from the batch dimension
        raw_image = raw_image[0]

        # Make sure it's contiguous in memory
        raw_image = np.ascontiguousarray(raw_image)

        # Create PIL image
        pil_img = Image.fromarray(raw_image, mode="RGB")

        if filename is None:
            # Generate output filename based on prompt and seed
            prompt_slug = self.prompt[:20].replace(" ", "_").replace("'", "").replace('"', '').replace(',', '').replace('.', '')
            filename = f"sd21_{prompt_slug}_seed{self.seed}.png"

        pil_img.save(filename)
        print(f"Saved {filename}")
        return filename
