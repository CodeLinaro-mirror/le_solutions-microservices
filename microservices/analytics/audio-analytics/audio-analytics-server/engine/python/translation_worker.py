# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#!/usr/bin/env python3
"""
Translation Worker Process

Each worker process handles one specific model to avoid C++ engine corruption.
"""

import sys
import json
import multiprocessing
import time
from translate_wrapper import TranslationWrapper

class TranslationWorker:
    def __init__(self, model_path: str, input_lang: str, output_lang: str):
        self.model_path = model_path.encode('utf-8')
        self.input_lang = input_lang.encode('utf-8') 
        self.output_lang = output_lang.encode('utf-8')
        self.wrapper = None
        self.worker_id = f"{input_lang}->{output_lang}"
        
    def initialize(self):
        """Initialize the translation wrapper for this specific model."""
        try:
            print(f"[Worker {self.worker_id}] Initializing with model: {self.model_path.decode()}")
            self.wrapper = TranslationWrapper(
                model_path=self.model_path,
                input_lang=self.input_lang,
                output_lang=self.output_lang
            )
            print(f"[Worker {self.worker_id}] Initialization successful")
            return True
        except Exception as e:
            print(f"[Worker {self.worker_id}] Initialization failed: {e}")
            import traceback
            traceback.print_exc()
            return False
    
    def process_text(self, text: str):
        """Process a single text and return the result."""
        if not self.wrapper:
            error_msg = "Worker not initialized"
            print(f"[Worker {self.worker_id}] ✗ ERROR: {error_msg}")
            return {"error": error_msg, "result": None}
            
        try:
            print(f"[Worker {self.worker_id}] ⚙ Processing: {text[:50]}...")
            
            # Storage for results - use list to collect all result chunks
            result_data = {"result_parts": [], "error": None, "done": False}
            
            def on_result(translated_text):
                """Callback receives translation result chunks."""
                print(f"[Worker {self.worker_id}] 📥 Result callback: '{translated_text}'")
                result_data["result_parts"].append(translated_text)
            
            def on_done():
                """Callback signals translation is complete."""
                print(f"[Worker {self.worker_id}] ✓ Done callback received")
                result_data["done"] = True
            
            def on_error(error_code):
                """Callback signals an error occurred."""
                error_msg = f"Translation engine error code: {error_code}"
                print(f"[Worker {self.worker_id}] ✗ Error callback: {error_msg}")
                result_data["error"] = error_msg
            
            # Process with callbacks
            print(f"[Worker {self.worker_id}] Calling translation engine...")
            ret = self.wrapper.process_with_cb(text, on_result, on_done, on_error, timeout=15.0)
            print(f"[Worker {self.worker_id}] Translation engine returned: {ret}")
            
            # Check return code
            if ret != 0:
                error_msg = f"Translation engine failed with return code {ret}"
                print(f"[Worker {self.worker_id}] ✗ {error_msg}")
                return {"error": error_msg, "result": None}
            
            # Check for error callback
            if result_data["error"]:
                print(f"[Worker {self.worker_id}] ✗ Error from callback: {result_data['error']}")
                return {"error": result_data["error"], "result": None}
            
            # Check if we got results
            if not result_data["result_parts"]:
                error_msg = "No translation result received from engine (callbacks may not have fired)"
                print(f"[Worker {self.worker_id}] ✗ {error_msg}")
                print(f"[Worker {self.worker_id}]    Callback state: done={result_data['done']}, parts={len(result_data['result_parts'])}")
                return {"error": error_msg, "result": None}
            
            # Concatenate all result parts
            full_result = ' '.join(result_data["result_parts"]).strip()
            print(f"[Worker {self.worker_id}] ✓ Translation successful: '{full_result[:100]}...'")
            print(f"[Worker {self.worker_id}]    Received {len(result_data['result_parts'])} result chunks")
            
            return {"error": None, "result": full_result}
            
        except Exception as e:
            error_msg = f"Exception during processing: {e}"
            print(f"[Worker {self.worker_id}] ✗ {error_msg}")
            import traceback
            traceback.print_exc()
            return {"error": error_msg, "result": None}
    
    def cleanup(self):
        """Clean up resources."""
        if self.wrapper:
            try:
                self.wrapper.close()
            except:
                pass
            self.wrapper = None

def worker_main(model_path: str, input_lang: str, output_lang: str, request_queue, response_queue):
    """Main function for worker process."""
    worker_id = f"{input_lang}->{output_lang}"
    print(f"[Worker {worker_id}] Starting worker process")
    
    # Initialize worker
    worker = TranslationWorker(model_path, input_lang, output_lang)
    if not worker.initialize():
        print(f"[Worker {worker_id}] Failed to initialize, exiting")
        return
    
    print(f"[Worker {worker_id}] Ready to process requests")
    
    try:
        while True:
            try:
                # Wait for request indefinitely (blocking)
                # No timeout - worker will wait until work arrives or shutdown signal is sent
                request = request_queue.get()
                
                if request is None:  # Shutdown signal
                    print(f"[Worker {worker_id}] ✓ Received shutdown signal, exiting gracefully")
                    break
                
                request_id = request.get("id")
                text = request.get("text")
                
                print(f"[Worker {worker_id}] ⚙ Processing request {request_id}: '{text[:50]}...'")
                
                # Process the text
                result = worker.process_text(text)
                
                # Send response
                response = {
                    "id": request_id,
                    "result": result["result"],
                    "error": result["error"]
                }
                response_queue.put(response)
                
                if result["error"]:
                    print(f"[Worker {worker_id}] ✗ Request {request_id} failed: {result['error']}")
                else:
                    print(f"[Worker {worker_id}] ✓ Request {request_id} completed successfully")
                
            except Exception as e:
                print(f"[Worker {worker_id}] ✗ ERROR in main loop: {e}")
                import traceback
                traceback.print_exc()
                break
                
    finally:
        print(f"[Worker {worker_id}] Cleaning up and exiting")
        worker.cleanup()

if __name__ == "__main__":
    # For testing
    if len(sys.argv) != 4:
        print("Usage: translation_worker.py <model_path> <input_lang> <output_lang>")
        sys.exit(1)
    
    model_path, input_lang, output_lang = sys.argv[1:4]
    
    # Create queues for testing
    import multiprocessing as mp
    request_queue = mp.Queue()
    response_queue = mp.Queue()
    
    # Test the worker
    worker_main(model_path, input_lang, output_lang, request_queue, response_queue)
