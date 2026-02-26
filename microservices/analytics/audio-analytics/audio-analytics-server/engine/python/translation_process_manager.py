# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#!/usr/bin/env python3
"""
Translation Process Manager

Manages separate worker processes for each translation model to avoid C++ engine corruption.
"""

import multiprocessing
import threading
import time
import uuid
from typing import Dict, Optional
from translation_worker import worker_main

class TranslationProcessManager:
    def __init__(self):
        self.workers: Dict[str, dict] = {}  # worker_key -> worker_info
        self.pending_requests: Dict[str, dict] = {}  # request_id -> request_info
        self.response_thread = None
        self.shutdown_event = threading.Event()
        
    def get_worker_key(self, model_path: str, input_lang: str, output_lang: str) -> str:
        """Generate a unique key for a worker configuration."""
        # Extract model name from path for cleaner key
        model_name = model_path.split('/')[-1].replace('.64_bit.qnn_v2.33.0.qnn', '')
        return f"{model_name}_{input_lang}_{output_lang}"
    
    def start_worker(self, model_path: str, input_lang: str, output_lang: str) -> bool:
        """Start a worker process for the given model configuration."""
        worker_key = self.get_worker_key(model_path, input_lang, output_lang)
        
        if worker_key in self.workers:
            print(f"[ProcessManager] ℹ Worker '{worker_key}' already running (PID: {self.workers[worker_key]['process'].pid})")
            return True
        
        print(f"[ProcessManager] 🚀 Starting new worker for '{worker_key}'...")
        print(f"[ProcessManager]    Model: {model_path}")
        print(f"[ProcessManager]    Translation: {input_lang} → {output_lang}")
        
        try:
            # Create communication queues
            request_queue = multiprocessing.Queue()
            response_queue = multiprocessing.Queue()
            
            # Start worker process
            process = multiprocessing.Process(
                target=worker_main,
                args=(model_path, input_lang, output_lang, request_queue, response_queue),
                name=f"TranslationWorker-{worker_key}"
            )
            process.start()
            
            # Store worker info
            self.workers[worker_key] = {
                "process": process,
                "request_queue": request_queue,
                "response_queue": response_queue,
                "model_path": model_path,
                "input_lang": input_lang,
                "output_lang": output_lang,
                "started_at": time.time()
            }
            
            # Start response monitoring thread if not already running
            if self.response_thread is None:
                print(f"[ProcessManager] 🔍 Starting response monitor thread...")
                self.response_thread = threading.Thread(target=self._response_monitor, daemon=True)
                self.response_thread.start()
            
            print(f"[ProcessManager] ✓ Worker '{worker_key}' started successfully (PID: {process.pid})")
            return True
            
        except Exception as e:
            print(f"[ProcessManager] ✗ FAILED to start worker '{worker_key}': {e}")
            import traceback
            traceback.print_exc()
            return False
    
    def translate_text(self, text: str, model_path: str, input_lang: str, output_lang: str, timeout: float = 10.0) -> dict:
        """Translate text using the appropriate worker process."""
        worker_key = self.get_worker_key(model_path, input_lang, output_lang)
        
        # Ensure worker exists
        if not self.start_worker(model_path, input_lang, output_lang):
            error_msg = f"Failed to start worker for {worker_key}"
            print(f"[ProcessManager] ✗ {error_msg}")
            return {"error": error_msg, "result": None}
        
        worker_info = self.workers[worker_key]
        
        # Check if process is still alive
        if not worker_info["process"].is_alive():
            print(f"[ProcessManager] ⚠ Worker '{worker_key}' died unexpectedly, restarting...")
            self.stop_worker(worker_key)
            if not self.start_worker(model_path, input_lang, output_lang):
                error_msg = f"Failed to restart worker for {worker_key}"
                print(f"[ProcessManager] ✗ {error_msg}")
                return {"error": error_msg, "result": None}
            worker_info = self.workers[worker_key]
            print(f"[ProcessManager] ✓ Worker '{worker_key}' restarted successfully")
        
        # Generate request ID (shorter for readability)
        request_id = str(uuid.uuid4())[:8]
    def translate_text(self, text: str, model_path: str, input_lang: str, output_lang: str, timeout: float = 10.0, retry_on_corruption: bool = True) -> dict:
        """Translate text using the appropriate worker process."""
        worker_key = self.get_worker_key(model_path, input_lang, output_lang)
        
        # Ensure worker exists
        if not self.start_worker(model_path, input_lang, output_lang):
            error_msg = f"Failed to start worker for {worker_key}"
            print(f"[ProcessManager] ✗ {error_msg}")
            return {"error": error_msg, "result": None}
        
        worker_info = self.workers[worker_key]
        
        # Check if process is still alive
        if not worker_info["process"].is_alive():
            print(f"[ProcessManager] ⚠ Worker '{worker_key}' died unexpectedly, restarting...")
            self.stop_worker(worker_key)
            if not self.start_worker(model_path, input_lang, output_lang):
                error_msg = f"Failed to restart worker for {worker_key}"
                print(f"[ProcessManager] ✗ {error_msg}")
                return {"error": error_msg, "result": None}
            worker_info = self.workers[worker_key]
            print(f"[ProcessManager] ✓ Worker '{worker_key}' restarted successfully")
        
        # Generate request ID (shorter for readability)
        request_id = str(uuid.uuid4())[:8]
        
        # Prepare request
        request = {
            "id": request_id,
            "text": text
        }
        
        # Store pending request
        result_event = threading.Event()
        self.pending_requests[request_id] = {
            "event": result_event,
            "response": None,
            "worker_key": worker_key
        }
        
        try:
            # Send request to worker
            print(f"[ProcessManager] Sending request {request_id} to worker {worker_key}")
            worker_info["request_queue"].put(request)
            
            # Wait for response with progress indication
            print(f"[ProcessManager] ⏳ Waiting for response (timeout: {timeout}s)...")
            if result_event.wait(timeout):
                response = self.pending_requests[request_id]["response"]
                
                # Check if the response indicates engine corruption
                # (process succeeded but no result - engine is corrupted)
                if response.get("error") and "Translation engine failed with return code -1" in response.get("error", ""):
                    print(f"[ProcessManager] ⚠ Worker '{worker_key}' appears corrupted (process succeeded but no result)")
                    
                    if retry_on_corruption:
                        print(f"[ProcessManager] 🔄 Killing corrupted worker and retrying...")
                        # Kill the corrupted worker
                        self.stop_worker(worker_key)
                        
                        # Retry the translation with a fresh worker (but don't retry again to avoid infinite loop)
                        print(f"[ProcessManager] � Retrying translation with fresh worker...")
                        return self.translate_text(text, model_path, input_lang, output_lang, timeout, retry_on_corruption=False)
                    else:
                        print(f"[ProcessManager] ✗ Worker corruption detected but retry already attempted")
                        return response
                
                if response.get("error"):
                    print(f"[ProcessManager] ✗ Request [{request_id}] failed: {response['error']}")
                else:
                    result_preview = response.get("result", "")[:50]
                    print(f"[ProcessManager] ✓ Request [{request_id}] completed: '{result_preview}...'")
                
                return response
            else:
                error_msg = f"Translation timed out after {timeout}s - worker may be overloaded or stuck"
                print(f"[ProcessManager] ⏱ ✗ Request [{request_id}] {error_msg}")
                print(f"[ProcessManager]    Worker '{worker_key}' status: alive={worker_info['process'].is_alive()}, PID={worker_info['process'].pid}")
                
                # If timeout, the worker might be stuck - consider killing it
                print(f"[ProcessManager] ⚠ Killing potentially stuck worker '{worker_key}'...")
                self.stop_worker(worker_key)
                
                return {"error": error_msg, "result": None}
                
        except Exception as e:
            print(f"[ProcessManager] Error processing request {request_id}: {e}")
            return {"error": str(e), "result": None}
            
        finally:
            # Clean up pending request
            if request_id in self.pending_requests:
                del self.pending_requests[request_id]
    
    def _response_monitor(self):
        """Monitor all worker response queues in a separate thread."""
        print("[ProcessManager] Response monitor thread started")
        
        while not self.shutdown_event.is_set():
            try:
                # Check each worker's response queue
                for worker_key, worker_info in list(self.workers.items()):
                    try:
                        response_queue = worker_info["response_queue"]
                        
                        # Non-blocking check for responses
                        try:
                            response = response_queue.get_nowait()
                            request_id = response["id"]
                            
                            print(f"[ProcessManager] Received response for request {request_id} from worker {worker_key}")
                            
                            # Find pending request and signal completion
                            if request_id in self.pending_requests:
                                self.pending_requests[request_id]["response"] = response
                                self.pending_requests[request_id]["event"].set()
                            else:
                                print(f"[ProcessManager] Received response for unknown request {request_id}")
                                
                        except:
                            # No response available, continue
                            pass
                            
                    except Exception as e:
                        print(f"[ProcessManager] Error checking responses for worker {worker_key}: {e}")
                
                # Small delay to prevent busy waiting
                time.sleep(0.01)
                
            except Exception as e:
                print(f"[ProcessManager] Error in response monitor: {e}")
                time.sleep(0.1)
        
        print("[ProcessManager] Response monitor thread stopped")
    
    def stop_worker(self, worker_key: str):
        """Stop a specific worker process."""
        if worker_key not in self.workers:
            return
        
        worker_info = self.workers[worker_key]
        process = worker_info["process"]
        
        print(f"[ProcessManager] Stopping worker {worker_key} (PID: {process.pid})")
        
        try:
            # Send shutdown signal
            worker_info["request_queue"].put(None)
            
            # Wait for graceful shutdown
            process.join(timeout=5.0)
            
            if process.is_alive():
                print(f"[ProcessManager] Force terminating worker {worker_key}")
                process.terminate()
                process.join(timeout=2.0)
                
                if process.is_alive():
                    print(f"[ProcessManager] Force killing worker {worker_key}")
                    process.kill()
                    process.join()
            
            print(f"[ProcessManager] Worker {worker_key} stopped")
            
        except Exception as e:
            print(f"[ProcessManager] Error stopping worker {worker_key}: {e}")
        
        finally:
            # Clean up
            del self.workers[worker_key]
    
    def stop_all_workers(self):
        """Stop all worker processes."""
        print("[ProcessManager] Stopping all workers...")
        
        # Signal shutdown
        self.shutdown_event.set()
        
        # Stop all workers
        for worker_key in list(self.workers.keys()):
            self.stop_worker(worker_key)
        
        # Wait for response thread to finish
        if self.response_thread and self.response_thread.is_alive():
            self.response_thread.join(timeout=2.0)
        
        print("[ProcessManager] All workers stopped")
    
    def get_worker_status(self) -> dict:
        """Get status of all workers."""
        status = {}
        for worker_key, worker_info in self.workers.items():
            status[worker_key] = {
                "alive": worker_info["process"].is_alive(),
                "pid": worker_info["process"].pid,
                "started_at": worker_info["started_at"],
                "model_path": worker_info["model_path"],
                "input_lang": worker_info["input_lang"],
                "output_lang": worker_info["output_lang"]
            }
        return status

# Global instance
_process_manager = None

def get_process_manager() -> TranslationProcessManager:
    """Get the global process manager instance."""
    global _process_manager
    if _process_manager is None:
        _process_manager = TranslationProcessManager()
    return _process_manager
