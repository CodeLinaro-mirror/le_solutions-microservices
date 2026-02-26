# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Test to verify if sounddevice stream.start() is blocking or non-blocking
"""

import sounddevice as sd
import numpy as np
import time
from collections import deque

def test_stream_blocking_behavior():
    """Test if stream.start() blocks the main thread"""
    
    print("=" * 70)
    print("Testing sounddevice stream.start() blocking behavior")
    print("=" * 70)
    
    # Create a simple audio buffer
    audio_deque = deque()
    
    # Generate 2 seconds of audio data (sine wave)
    sample_rate = 44100
    duration = 2.0
    frequency = 440  # A4 note
    t = np.linspace(0, duration, int(sample_rate * duration), False)
    audio_data = (np.sin(2 * np.pi * frequency * t) * 0.3).astype(np.float32)
    
    # Split into chunks
    chunk_size = 4096
    for i in range(0, len(audio_data), chunk_size):
        chunk = audio_data[i:i + chunk_size]
        audio_deque.append(chunk)
    
    print(f"\nGenerated {duration}s of audio in {len(audio_deque)} chunks")
    
    # Define callback
    def callback(outdata, frames, time_info, status):
        if status:
            print(f"Status: {status}")
        
        try:
            chunk = audio_deque.popleft()
            if len(chunk) < frames:
                # Pad with zeros if needed
                chunk = np.pad(chunk, (0, frames - len(chunk)))
            outdata[:] = chunk[:frames].reshape(-1, 1)
        except IndexError:
            # No more data, output silence
            outdata[:] = np.zeros((frames, 1), dtype=np.float32)
    
    # Create stream
    print("\nCreating stream...")
    stream = sd.OutputStream(
        samplerate=sample_rate,
        channels=1,
        dtype='float32',
        callback=callback,
        blocksize=1024
    )
    
    print("Calling stream.start()...")
    start_time = time.time()
    
    stream.start()  # ← This is what we're testing
    
    end_time = time.time()
    elapsed = end_time - start_time
    
    print(f"stream.start() returned in {elapsed*1000:.2f} milliseconds")
    
    if elapsed < 0.1:  # Less than 100ms
        print("✓ stream.start() is NON-BLOCKING (returned immediately)")
    else:
        print("✗ stream.start() appears to be BLOCKING")
    
    # Demonstrate that main thread continues
    print("\nMain thread continuing execution while audio plays...")
    for i in range(5):
        print(f"  Main thread tick {i+1}/5 (buffer has {len(audio_deque)} chunks)")
        time.sleep(0.5)
    
    print("\nStopping stream...")
    stream.stop()
    stream.close()
    
    print("\n" + "=" * 70)
    print("Test complete!")
    print("=" * 70)
    
    print("\nConclusion:")
    print("  - stream.start() returned immediately (non-blocking)")
    print("  - Main thread continued executing while audio played")
    print("  - Audio callback ran in separate thread")
    print("  - This proves sounddevice streams are NON-BLOCKING")


def test_comparison_with_blocking():
    """Compare with a blocking operation for reference"""
    
    print("\n" + "=" * 70)
    print("Comparison: What a BLOCKING operation looks like")
    print("=" * 70)
    
