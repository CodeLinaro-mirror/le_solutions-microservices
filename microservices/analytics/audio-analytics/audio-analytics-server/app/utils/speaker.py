# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Audio Speaker Module

This module provides functionality for playing audio through output devices using sounddevice.
It includes device detection, queue-based audio data management, and persistent stream
playback that idles silently when no data is queued.
"""

import sounddevice as sd
import queue
import numpy as np
import threading


class Speaker:
    """
    Audio speaker class that plays audio through a specified output device.
    
    Features:
    - Device discovery and selection by name
    - Queue-based audio data buffering
    - Persistent stream that idles silently between requests
    - Thread-safe playback control
    - Chunk-based audio streaming
    """

    def __init__(self, name, dtype="int16", refresh: bool = True):
        """
        Initialize the Speaker with audio parameters.
        
        Args:
            name (str): Partial or full name of the output device to use
            dtype (str): Data type for audio samples (default: 'int16')
        """
        if refresh:
            Speaker.refresh_devices()
        self.device_name = Speaker.get_speaker_by_name(name)
        self.sample_rate = Speaker.get_device_sample_rate(self.device_name)
        self.channels = Speaker.get_device_output_channels(self.device_name)
        self.dtype = dtype
        self.bytes_per_sample = np.dtype(dtype).itemsize

        self.audio_queue = queue.Queue()
        self.current_chunk = None
        self.current_chunk_index = 0
        
        self.stream = None
        self._stop_event = threading.Event()

        # Cache device parameters at creation time to avoid querying PortAudio later
        self.cached_sample_rate = Speaker.get_device_sample_rate(self.device_name)
        self.cached_channels = Speaker.get_device_output_channels(self.device_name)

    @staticmethod
    def list_output_devices(refresh: bool = True):
        """
        List all available audio output devices.

        Args:
            refresh: Whether to call refresh_devices() first.  Set to False when
                     a stream is already open — refresh_devices() calls
                     sd._terminate() which would kill the active stream.
        
        Returns:
            list: List of device information dictionaries with output capabilities only
        """
        if refresh:
            Speaker.refresh_devices()
        print("\nAvailable Output Devices:")
        devices = sd.query_devices()
        
        # Filter and return only devices with output capabilities
        output_devices = []
        for dev in devices:
            if dev["max_output_channels"] > 0:
                print(f"{dev['index']}: {dev['name']} - {dev['default_samplerate']}")
                output_devices.append(dev)
        
        if not output_devices:
            print("No output devices found!")
        
        return output_devices

    @staticmethod
    def get_device_info(device_name, key):
        """
        Get specific information about a device.
        
        Args:
            device_name (str): Name of the device to look up
            key (str): Information key to retrieve (e.g., 'default_samplerate')
            
        Returns:
            The requested device information, or None if not found
        """
        try:
            # Use no-args form — sd.query_devices(name) can return a tuple in some
            # sounddevice versions, but the no-args form always yields dict-like objects
            devices = sd.query_devices()
            for device in devices:
                if device["name"] == device_name:
                    return device[key]
            print(f"Device '{device_name}' not found")
            return None
        except Exception as e:
            print(f"Error getting device information: {e}")
            return None

    @staticmethod
    def get_device_sample_rate(device_name):
        """Get the default sample rate of a device."""
        return Speaker.get_device_info(device_name, "default_samplerate")

    @staticmethod
    def get_device_output_channels(device_name):
        """Get the maximum number of output channels for a device."""
        return Speaker.get_device_info(device_name, "max_output_channels")

    @staticmethod
    def get_speaker_by_name(name):
        """
        Find an output device by partial name match (case-insensitive).

        If *name* is None or empty, the system default output device is returned.

        Args:
            name (str | None): Partial or full device name to search for,
                               or None/empty to use the system default.

        Returns:
            str: Device name string if found, None otherwise

        Example:
            get_speaker_by_name("pulse") might return "pulse" or the full device name
            get_speaker_by_name(None) returns the system default output device name
        """
        # No name supplied — return the system default output device
        if not name:
            try:
                default_device = sd.query_devices(kind='output')
                return default_device['name']
            except Exception:
                # Fall back: first device with output channels
                for device in sd.query_devices():
                    if device["max_output_channels"] > 0:
                        return device["name"]
                return None

        devices = sd.query_devices()

        # Search for devices with output capability matching the name
        for device in devices:
            if device["max_output_channels"] > 0 and name.lower() in device["name"].lower():
                # Return the device name string so sd.query_devices(name) returns a dict
                return device["name"]

        # Device not found
        return None

    def get_device_name(self):
        """Get the name of the currently selected device."""
        return self.device_name

    @staticmethod
    def refresh_devices():
        """
        Refresh the audio device list.

        This is useful for detecting newly connected or disconnected devices.
        Terminates and reinitializes the PortAudio system.
        """
        sd._terminate()
        sd._initialize()

    def output_callback(self, outdata, frames, time, status):
        """
        Audio stream callback function called by sounddevice for each audio block.
        
        For a RawOutputStream, outdata is a flat bytes-like buffer of exactly
        frames * channels * bytes_per_sample bytes. We always fill it fully —
        silence for any unfilled portion — so PortAudio never sees garbage data.

        Args:
            outdata: Output audio buffer to fill with data
            frames: Number of frames requested
            time: Timing information
            status: Status flags indicating any issues
        """
        if status:
            print(f"Speaker status: {status}")
            # Do NOT return — still fill the buffer to avoid underflow cascade

        bytes_needed = frames * self.channels * self.bytes_per_sample

        # Build output from queued chunks, padding with silence if not enough data
        out = bytearray(bytes_needed)  # zero-filled = silence
        pos = 0

        while pos < bytes_needed:
            # Refill current chunk if exhausted
            if self.current_chunk is None:
                try:
                    self.current_chunk = self.audio_queue.get_nowait()
                    self.current_chunk_index = 0
                except queue.Empty:
                    break  # No more data — remainder stays silent

            available = len(self.current_chunk) - self.current_chunk_index
            to_copy = min(bytes_needed - pos, available)

            out[pos:pos + to_copy] = self.current_chunk[
                self.current_chunk_index : self.current_chunk_index + to_copy
            ]
            pos += to_copy
            self.current_chunk_index += to_copy

            if self.current_chunk_index >= len(self.current_chunk):
                self.current_chunk = None
                self.current_chunk_index = 0

        outdata[:] = bytes(out)

    def play_speaker_buffer(self, callback=None):
        """
        Start the persistent audio stream and block until stop() is called.

        The stream runs continuously, outputting silence when the queue is empty
        and playing audio as soon as chunks are added via add_to_buffer().
        This means the speaker stays alive across multiple TTS requests with no
        thread restart overhead between them.

        Args:
            callback (callable, optional): Custom callback function to use instead
                                          of the default output_callback.

        Should be run in a dedicated daemon thread.
        """
        self._stop_event.clear()
        try:
            stream = sd.RawOutputStream(
                samplerate=self.sample_rate,
                device=self.device_name,
                channels=self.channels,
                dtype=self.dtype,
                callback=callback if callback else self.output_callback,
                blocksize=2048,
            )
            self.stream = stream
            try:
                with self.stream:
                    # Block here until stop() sets the event
                    self._stop_event.wait()
            except Exception as e:
                # Stream may have been killed externally (e.g. sd._terminate())
                print(f"Speaker stream error: {e}")
        except Exception as e:
            print(f"Speaker stream open error: {e}")
        finally:
            # Always clear the stream reference so is_running() returns False
            self.stream = None
            print("Speaker stream stopped")

    def stop(self):
        """
        Signal the persistent stream to stop.

        Sets the stop event so that play_speaker_buffer() exits its wait and
        closes the stream. Any audio still in the queue will be discarded.
        """
        self.clear_buffer()
        self._stop_event.set()


    def add_to_buffer(self, chunk):
        """
        Add an audio chunk to the playback buffer.
        
        Args:
            chunk (bytes): Audio data chunk to add to the queue
        """
        self.audio_queue.put(chunk)

    def clear_buffer(self):
        """
        Clear all audio data from the playback buffer.
        
        This immediately removes all queued audio chunks, effectively
        stopping playback after the current chunk finishes.
        """
        with self.audio_queue.mutex:
            self.audio_queue.queue.clear()

    def is_running(self):
        """
        Return True if the persistent stream is currently open and active.

        Checks stream.active rather than just stream is not None so that a
        stream killed externally by sd._terminate() is correctly reported as
        not running.
        """
        if self.stream is None:
            return False
        try:
            return self.stream.active
        except Exception:
            return False
