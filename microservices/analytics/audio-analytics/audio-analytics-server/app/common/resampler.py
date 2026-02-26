#!/usr/bin/env python3
# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Audio resampler module for audio-analytics-server.

This module provides functionality to resample audio data to a target sample rate
and channel count without relying on external libraries.
"""

import numpy as np
import io
import wave
import struct
from typing import Tuple, Optional, Union, List


class AudioResampler:
    """
    A simple audio resampler that doesn't rely on third-party libraries.
    
    This class provides methods to resample audio data to a target sample rate
    and channel count using linear interpolation.
    """
    
    def __init__(self):
        """Initialize the AudioResampler."""
        pass
    
    @staticmethod
    def resample(
        audio_data: np.ndarray,
        original_sample_rate: int,
        target_sample_rate: int,
        original_channels: int = 1,
        target_channels: int = 1
    ) -> np.ndarray:
        """
        Resample audio data to a target sample rate and channel count.
        
        Args:
            audio_data: Input audio data as numpy array
            original_sample_rate: Original sample rate in Hz
            target_sample_rate: Target sample rate in Hz
            original_channels: Number of channels in the input audio
            target_channels: Number of channels in the output audio
            
        Returns:
            Resampled audio data as numpy array
        """
        # Handle channel conversion first
        if original_channels != target_channels:
            audio_data = AudioResampler._convert_channels(
                audio_data, original_channels, target_channels
            )
        
        # If sample rates are the same, return the audio data
        if original_sample_rate == target_sample_rate:
            return audio_data

        # Calculate the resampling ratio
        ratio = original_sample_rate / target_sample_rate
        
        # Calculate the output length
        output_length = int(len(audio_data) / ratio)
        
        # Create the output array
        resampled = np.zeros((output_length, target_channels), dtype=audio_data.dtype)
        
        # Perform linear interpolation resampling
        for i in range(output_length):
            # Calculate the exact position in the input array
            src_idx_float = i * ratio
            src_idx = int(src_idx_float)
            next_idx = min(src_idx + 1, len(audio_data) - 1)
            
            # Calculate the interpolation factor
            alpha = src_idx_float - src_idx
            
            # Linear interpolation
            resampled[i] = (1 - alpha) * audio_data[src_idx] + alpha * audio_data[next_idx]
        
        return resampled
    
    @staticmethod
    def _convert_channels(
        audio_data: np.ndarray,
        original_channels: int,
        target_channels: int
    ) -> np.ndarray:
        """
        Convert audio data between different channel counts.
        
        Args:
            audio_data: Input audio data as numpy array
            original_channels: Number of channels in the input audio
            target_channels: Number of channels in the output audio
            
        Returns:
            Audio data with the target number of channels
        """
        # Ensure audio_data is 2D (samples, channels)
        if len(audio_data.shape) == 1 and original_channels == 1:
            audio_data = audio_data.reshape(-1, 1)
        
        # Mono to stereo: duplicate the mono channel
        if original_channels == 1 and target_channels == 2:
            return np.column_stack((audio_data, audio_data))
        
        # Stereo to mono: average the stereo channels
        elif original_channels == 2 and target_channels == 1:
            return np.mean(audio_data, axis=1, keepdims=True)
        
        # More complex channel conversions
        elif target_channels > original_channels:
            # Duplicate channels and adjust volume
            result = np.zeros((len(audio_data), target_channels), dtype=audio_data.dtype)
            for i in range(target_channels):
                result[:, i] = audio_data[:, i % original_channels]
            return result
        
        elif original_channels > target_channels:
            # Average groups of channels
            result = np.zeros((len(audio_data), target_channels), dtype=audio_data.dtype)
            channels_per_group = original_channels // target_channels
            for i in range(target_channels):
                start_ch = i * channels_per_group
                end_ch = start_ch + channels_per_group
                result[:, i] = np.mean(audio_data[:, start_ch:end_ch], axis=1)
            return result
        
        # Same number of channels, return as is
        return audio_data
    
    @staticmethod
    def resample_wav_file(
        wav_data: bytes,
        target_sample_rate: int = 16000,
        target_channels: int = 1
    ) -> bytes:
        """
        Resample a WAV file to a target sample rate and channel count.
        
        Args:
            wav_data: Input WAV file data as bytes
            target_sample_rate: Target sample rate in Hz
            target_channels: Number of channels in the output audio
            
        Returns:
            Resampled WAV file data as bytes
        """
        # Read the WAV file
        with io.BytesIO(wav_data) as wav_io:
            with wave.open(wav_io, 'rb') as wav_file:
                # Get the WAV file parameters
                original_channels = wav_file.getnchannels()
                original_sample_rate = wav_file.getframerate()
                sample_width = wav_file.getsampwidth()
                
                # Read the audio data
                audio_bytes = wav_file.readframes(wav_file.getnframes())
        
        # Convert bytes to numpy array
        if sample_width == 1:  # 8-bit unsigned
            audio_data = np.frombuffer(audio_bytes, dtype=np.uint8)
            audio_data = audio_data.astype(np.float32) / 128.0 - 1.0
        elif sample_width == 2:  # 16-bit signed
            audio_data = np.frombuffer(audio_bytes, dtype=np.int16)
            audio_data = audio_data.astype(np.float32) / 32768.0
        elif sample_width == 4:  # 32-bit signed
            audio_data = np.frombuffer(audio_bytes, dtype=np.int32)
            audio_data = audio_data.astype(np.float32) / 2147483648.0
        else:
            raise ValueError(f"Unsupported sample width: {sample_width}")
        
        # Reshape to (samples, channels)
        if original_channels > 1:
            audio_data = audio_data.reshape(-1, original_channels)
        else:
            audio_data = audio_data.reshape(-1, 1)
        
        # Resample the audio data
        resampled = AudioResampler.resample(
            audio_data,
            original_sample_rate,
            target_sample_rate,
            original_channels,
            target_channels
        )
        
        # Convert back to bytes
        if sample_width == 1:  # 8-bit unsigned
            resampled = (resampled * 128 + 128).astype(np.uint8)
            resampled_bytes = resampled.tobytes()
        elif sample_width == 2:  # 16-bit signed
            resampled = (resampled * 32767).astype(np.int16)
            resampled_bytes = resampled.tobytes()
        elif sample_width == 4:  # 32-bit signed
            resampled = (resampled * 2147483647).astype(np.int32)
            resampled_bytes = resampled.tobytes()
        
        # Create a new WAV file
        with io.BytesIO() as wav_io:
            with wave.open(wav_io, 'wb') as wav_file:
                wav_file.setnchannels(target_channels)
                wav_file.setsampwidth(sample_width)
                wav_file.setframerate(target_sample_rate)
                wav_file.writeframes(resampled_bytes)
            
            return wav_io.getvalue()
    
    @staticmethod
    def resample_audio_stream(
        audio_chunk: bytes,
        original_sample_rate: int,
        target_sample_rate: int,
        original_channels: int = 1,
        target_channels: int = 1,
        sample_width: int = 2  # Default to 16-bit
    ) -> bytes:
        """
        Resample an audio chunk from a stream.
        
        Args:
            audio_chunk: Input audio chunk as bytes
            original_sample_rate: Original sample rate in Hz
            target_sample_rate: Target sample rate in Hz
            original_channels: Number of channels in the input audio
            target_channels: Number of channels in the output audio
            sample_width: Sample width in bytes (1=8-bit, 2=16-bit, 4=32-bit)
            
        Returns:
            Resampled audio chunk as bytes
        """
        # Convert bytes to numpy array
        if sample_width == 1:  # 8-bit unsigned
            audio_data = np.frombuffer(audio_chunk, dtype=np.uint8)
            audio_data = audio_data.astype(np.float32) / 128.0 - 1.0
        elif sample_width == 2:  # 16-bit signed
            audio_data = np.frombuffer(audio_chunk, dtype=np.int16)
            audio_data = audio_data.astype(np.float32) / 32768.0
        elif sample_width == 4:  # 32-bit signed
            audio_data = np.frombuffer(audio_chunk, dtype=np.int32)
            audio_data = audio_data.astype(np.float32) / 2147483648.0
        else:
            raise ValueError(f"Unsupported sample width: {sample_width}")
        
        # Reshape to (samples, channels)
        if original_channels > 1:
            audio_data = audio_data.reshape(-1, original_channels)
        else:
            audio_data = audio_data.reshape(-1, 1)
        
        # Resample the audio data
        resampled = AudioResampler.resample(
            audio_data,
            original_sample_rate,
            target_sample_rate,
            original_channels,
            target_channels
        )
        
        # Convert back to bytes
        if sample_width == 1:  # 8-bit unsigned
            resampled = (resampled * 128 + 128).astype(np.uint8)
            return resampled.tobytes()
        elif sample_width == 2:  # 16-bit signed
            resampled = (resampled * 32767).astype(np.int16)
            return resampled.tobytes()
        elif sample_width == 4:  # 32-bit signed
            resampled = (resampled * 2147483647).astype(np.int32)
            return resampled.tobytes()


def resample_audio(
    audio_data: Union[bytes, np.ndarray],
    original_sample_rate: int,
    target_sample_rate: int = 16000,
    original_channels: int = 1,
    target_channels: int = 1,
    is_wav_file: bool = False,
    sample_width: int = 2
) -> bytes:
    """
    Convenience function to resample audio data.
    
    Args:
        audio_data: Input audio data as bytes or numpy array
        original_sample_rate: Original sample rate in Hz
        target_sample_rate: Target sample rate in Hz (default: 16000)
        original_channels: Number of channels in the input audio (default: 1)
        target_channels: Number of channels in the output audio (default: 1)
        is_wav_file: Whether the input is a WAV file (default: False)
        sample_width: Sample width in bytes for non-WAV data (default: 2)
        
    Returns:
        Resampled audio data as bytes
    """
    resampler = AudioResampler()
    
    if is_wav_file:
        if not isinstance(audio_data, bytes):
            raise TypeError("WAV file data must be bytes")
        return resampler.resample_wav_file(
            audio_data, target_sample_rate, target_channels
        )
    
    if isinstance(audio_data, bytes):
        return resampler.resample_audio_stream(
            audio_data,
            original_sample_rate,
            target_sample_rate,
            original_channels,
            target_channels,
            sample_width
        )
    
    # If audio_data is a numpy array, resample it and convert to bytes
    resampled = resampler.resample(
        audio_data,
        original_sample_rate,
        target_sample_rate,
        original_channels,
        target_channels
    )
    
    # Convert to 16-bit PCM by default
    resampled = (resampled * 32767).astype(np.int16)
    return resampled.tobytes()


# Example usage
if __name__ == "__main__":
    # Create a simple sine wave at 44.1kHz stereo
    duration = 1.0  # seconds
    sample_rate = 44100
    t = np.linspace(0, duration, int(sample_rate * duration), endpoint=False)
    sine_wave = np.sin(2 * np.pi * 440 * t)  # 440 Hz sine wave
    stereo_sine = np.column_stack((sine_wave, sine_wave))  # Convert to stereo
    
    # Resample to 16kHz mono
    resampled = AudioResampler.resample(
        stereo_sine, sample_rate, 16000, original_channels=2, target_channels=1
    )
    
    print(f"Original shape: {stereo_sine.shape}")
    print(f"Resampled shape: {resampled.shape}")
    print("Resampling successful!")