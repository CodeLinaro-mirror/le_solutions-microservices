# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Audio Recorder Module

This module provides functionality for recording audio from input devices using sounddevice.
It includes device detection, queue-based audio data management, and automatic device 
disconnection detection.
"""

from typing import Optional, List, Dict, Any, Callable
import sounddevice as sd
import asyncio
import queue
import threading
import numpy as np
import time


class Recorder:
    """
    Audio recorder class that captures audio from a specified input device.
    
    Features:
    - Device discovery and selection by name
    - Queue-based audio data buffering
    - Automatic device disconnection detection
    - Thread-safe recording control
    - Records data as raw pcm
    """

    def __init__(self, device_name: str, sample_rate: int, channels: int, dtype: str, recording_bytes: int = 8192, refresh: bool = True) -> None:
        """
        Initialize the Recorder with audio parameters.
        
        Args:
            device_name: Partial or full name of the input device to use
            sample_rate: Sample rate in Hz (e.g., 44100, 48000)
            channels: Number of audio channels (1 for mono, 2 for stereo)
            dtype: Data type for audio samples (e.g., 'int16', 'float32')
            recording_bytes: number of bytes to put in to queue at a time
            refresh: Whether to call refresh_devices() before querying devices.
                     Set to False when the caller has already refreshed (or when
                     another PortAudio stream — e.g. the TTS speaker — is active,
                     since refresh_devices() calls sd._terminate() which kills it).
        """
        if refresh:
            Recorder.refresh_devices()
        self.device_name: Optional[str] = Recorder.get_mic_by_name(device_name)
        
        # If device not found, raise an error
        if self.device_name is None:
            available_devices = Recorder.list_input_devices()
            device_names = [dev["name"] for dev in available_devices if dev["max_input_channels"] > 0]
            raise ValueError(
                f"Audio input device '{device_name}' not found. "
                f"Available input devices: {device_names if device_names else 'None'}"
            )
        
        self.sample_rate: int = sample_rate
        self.channels: int = channels
        self.dtype: str = dtype
        self.bytes_per_sample: int = np.dtype(dtype).itemsize

        self.stream: Optional[sd.RawInputStream] = None
        self.queue: queue.Queue[bytes] = queue.Queue()
        self.stream_event: threading.Event = threading.Event()

        self.recording_bytes: int = recording_bytes
        self.curr_chunk: bytes = b""
        # Counter incremented on every callback invocation — used by the
        # monitoring loop to detect device disconnection reliably even when
        # curr_chunk is always 0 (which happens when blocksize == recording_bytes).
        self._callback_count: int = 0

    @staticmethod
    def list_input_devices() -> List[Dict[str, Any]]:
        """
        List all available audio input devices.
        
        Returns:
            List of device information dictionaries
        """
        Recorder.refresh_devices()
        print("\nAvailable Input Devices:")
        devices = sd.query_devices()
        
        # Filter and return only devices with input capabilities
        input_devices = []
        for dev in devices:
            if dev["max_input_channels"] > 0:
                print(f"{dev['index']}: {dev['name']} - {dev['default_samplerate']}")
                input_devices.append(dev)
        
        if not input_devices:
            print("No input devices found!")
        
        return input_devices

    @staticmethod
    def get_device_info(device_name: str, key: str) -> Optional[Any]:
        """
        Get specific information about a device.
        
        Args:
            device_name: Name or index of the device
            key: Information key to retrieve (e.g., 'default_samplerate')
            
        Returns:
            The requested device information, or None if error occurs
        """
        try:
            device_info = sd.query_devices(device_name)
            value = device_info[key]
            return value
        except (ValueError, sd.PortAudioError) as e:
            print(f"Error getting device information: {e}")
            return None

    @staticmethod
    def get_device_sample_rate(device_name: str) -> Optional[float]:
        """Get the default sample rate of a device."""
        return Recorder.get_device_info(device_name, "default_samplerate")

    @staticmethod
    def get_device_input_channels(device_name: str) -> Optional[int]:
        """Get the maximum number of input channels for a device."""
        return Recorder.get_device_info(device_name, "max_input_channels")

    @staticmethod
    def refresh_devices() -> None:
        """
        Refresh the audio device list.

        This is useful for detecting newly connected or disconnected devices.
        Terminates and reinitializes the PortAudio system.

        **IMPORTANT**: Do not call this function while this is an active stream
        """
        sd._terminate()
        sd._initialize()

    @staticmethod
    def get_mic_by_name(name: str) -> Optional[str]:
        """
        Find an input device by partial name match (case-insensitive).

        If *name* is None or empty, the system default input device is returned.

        Args:
            name: Partial or full device name to search for, or None/empty to
                  use the system default input device.

        Returns:
            Full device name if found, None otherwise

        Example:
            get_mic_by_name("usb") might return "USB Audio Device"
            get_mic_by_name(None) returns the system default input device name
        """
        # No name supplied — return the system default input device
        if not name:
            try:
                default_device = sd.query_devices(kind='input')
                if default_device and default_device["max_input_channels"] > 0:
                    print(f"Using default input device: {default_device['name']}")
                    return default_device["name"]
            except Exception as e:
                print(f"Error getting default input device: {e}")
            # Fall back: first device with input channels
            for device in sd.query_devices():
                if device["max_input_channels"] > 0:
                    return device["name"]
            return None

        devices = sd.query_devices()

        # Search for devices with input capability matching the name
        for index, device in enumerate(devices):
            if device["max_input_channels"] > 0 and name.lower() in device["name"].lower():
                print(f"Found matching device: {device['name']}")
                return device["name"]

        # If no exact match, try to find default input device
        try:
            default_device = sd.query_devices(kind='input')
            if default_device and default_device["max_input_channels"] > 0:
                print(f"Using default input device: {default_device['name']}")
                return default_device["name"]
        except Exception as e:
            print(f"Error getting default input device: {e}")

        # Device not found
        print(f"No input device found matching '{name}'")
        return None

    def get_device_name(self) -> Optional[str]:
        """Get the name of the currently selected device."""
        return self.device_name
    
    def get_num_recording_bytes(self) -> int:
        """Get the number of recording bytes per chunk"""
        return self.recording_bytes

    def callback(self, indata: np.ndarray, frames: int, time: Any, status: sd.CallbackFlags) -> None:
        """
        Audio stream callback function called by sounddevice for each audio block.
        
        This function is called in a separate thread by the audio system.
        
        Args:
            indata: Input audio data as numpy array
            frames: Number of frames in this block
            time: Timing information
            status: Status flags indicating any issues
        """
        if status:
            print("Status:", status)
            if not status.input_overflow:
                return

        self._callback_count += 1

        if self.stream_event.is_set():
            # Add last bit of audio only if it's substantial (> 1KB)
            if len(self.curr_chunk) > 1024:
                self.queue.put(self.curr_chunk)
            raise sd.CallbackStop

        self.curr_chunk += bytes(indata)

        if len(self.curr_chunk) >= self.recording_bytes:
            temp_chunk = self.curr_chunk[self.recording_bytes:]
            self.queue.put(self.curr_chunk[:self.recording_bytes])
            self.curr_chunk = temp_chunk

    def start_recording(self) -> None:
        """
        Start recording audio from the device.
        
        This method blocks until recording is stopped or device is disconnected.
        It includes automatic device disconnection detection by monitoring the
        queue size - if no new data arrives for 5 seconds, it assumes the device
        was unplugged.
        
        Should be run in a separate thread to avoid blocking the main thread.
        """
        try:
            event = threading.Event()

            # Set blocksize to match recording_bytes so the callback fires once per
            # chunk rather than hundreds of times per second with a tiny default
            # blocksize.  Fewer, larger callbacks dramatically reduce input_overflow.
            blocksize = self.recording_bytes // (self.bytes_per_sample * self.channels)

            # Create audio input stream
            stream = sd.RawInputStream(
                samplerate=self.sample_rate,
                device=self.device_name,
                channels=self.channels,
                dtype=self.dtype,
                callback=self.callback,
                finished_callback=event.set,
                blocksize=blocksize
            )

            self.stream = stream
            
            with self.stream:
                last_callback_count = self._callback_count
                no_data_count = 0

                while not event.is_set():
                    # Use the callback counter rather than curr_chunk length.
                    # When blocksize == recording_bytes the chunk is enqueued
                    # immediately after every callback, so curr_chunk is always
                    # 0 at check time — giving false "no data" positives.
                    current_callback_count = self._callback_count
                    if current_callback_count != last_callback_count:
                        no_data_count = 0
                    else:
                        no_data_count += 1

                    # If no new data for 5 seconds, device might be unplugged
                    if no_data_count > 5:
                        print(f"No data received for {no_data_count} seconds - device may be unplugged!")
                        self.stop_recording()
                        break

                    last_callback_count = current_callback_count
                    time.sleep(1)

            print(f"Stream with device ({self.device_name}) has stopped recording")
            
        except Exception as e:
            print("Error:", e)
            self.stop_recording()

    def get_audio_data(self) -> Optional[bytes]:
        """
        Get the next chunk of audio data from the queue (non-blocking).
        
        Returns:
            Audio data chunk, or None if queue is empty
            
        Note:
            This method does not block. If no data is available, it returns None.
        """
        try:
            if not self.queue.empty():
                return self.queue.get_nowait()
            return None
        except queue.Empty:
            return None
    
    def get_all_audio_data(self) -> bytes:
        """
        Get all audio data from the queue joined into a single bytes object at this moment in time
        
        Returns:
            All audio data concatenated together, or empty bytes if queue is empty
        """
        if self.queue.empty():
            return b''

        queue_len = self.queue.qsize()

        audio_chunks = []
        for _ in range(0, queue_len):
            try:
                chunk = self.queue.get_nowait()
                audio_chunks.append(chunk)
            except queue.Empty:
                break

        # Join all chunks into a single bytes object
        return b''.join(audio_chunks)

    def queue_empty(self) -> bool:
        """
        Checks if queue is empty
        """
        return self.queue.empty()

    def clear_queue(self) -> None:
        """
        Clearing queue, if we no longer want data inside queue
        """
        with self.queue.mutex:
            self.queue.queue.clear()

    def stop_recording(self) -> None:
        """
        Signal the recording to stop.
        
        Sets the stream_event which will cause the callback to raise CallbackStop
        and the monitoring loop in start_recording to exit.
        """
        self.stream_event.set()

    def reset(self) -> None:
        """
        Reset internal state so the recorder can be restarted without recreating the object.
        Clears the stop event, queue, current chunk buffer and stream reference.
        Must be called after the previous recording thread has fully stopped.
        """
        self.stream_event.clear()
        self.stream = None
        self.curr_chunk = b""
        with self.queue.mutex:
            self.queue.queue.clear()

    def done_recording(self) -> bool:
        """
        Check if recording has finished.
        
        Returns:
            True if recording is done, False if still recording
        """
        return self.stream_event.is_set()


if __name__ == "__main__":
    """
    Run this script directly to list available audio input devices and test device lookups.
    """
    print("=== Audio Input Device Information ===")
    
    try:
        # List all available input devices using the function
        input_devices = Recorder.list_input_devices()
        
        print(f"\nFound {len(input_devices)} input devices")
        
        # Test device name lookups
        test_names = ["pulse", "default", "alsa", "usb", "microphone", "audio", "built-in", "internal"]
        print("\n=== Testing Device Name Lookups ===")
        
        for name in test_names:
            print(f"\nTesting '{name}':")
            found_device = Recorder.get_mic_by_name(name)
            if found_device:
                print(f"  ✓ Found: '{found_device}'")
            else:
                print(f"  ✗ Not found")
        
        # Show default input device
        print("\n=== Default Input Device ===")
        try:
            import sounddevice as sd
            default_device = sd.query_devices(kind='input')
            if default_device:
                print(f"Default input device: '{default_device['name']}'")
                print(f"Sample rate: {default_device['default_samplerate']} Hz")
                print(f"Channels: {default_device['max_input_channels']}")
            else:
                print("No default input device found")
        except Exception as e:
            print(f"Error getting default device: {e}")
            
    except Exception as e:
        print(f"Error listing devices: {e}")
        import traceback
        traceback.print_exc()
