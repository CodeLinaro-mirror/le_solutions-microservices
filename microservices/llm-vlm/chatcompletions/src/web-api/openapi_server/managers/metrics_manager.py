# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Metrics Manager

Singleton manager for tracking system resources and model-specific inference metrics.
Provides moving averages for pipeline stage measurements across both LLM and VLM models.

Tracked metrics per model:
  - avg_preprocessing_time_ms   : Pre-processing encode/resize time (VLM only)
  - avg_ttft_ms                 : Time to First Token
  - avg_tokens_per_second       : Steady-state token generation rate
  - avg_stream_latency_ms       : Average inter-token stream latency
  - avg_total_pipeline_latency_ms : End-to-end total pipeline latency
"""

import threading
import time
from collections import deque
from typing import Dict, Optional, List
import psutil

from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

# Sliding window size for moving averages
_WINDOW_SIZE = 100


class ModelMetrics:
    """
    Tracks pipeline-stage metrics for a specific model using a sliding window.
    Works for both LLM (no preprocessing) and VLM (with preprocessing) models.
    """

    def __init__(self, model_id: str, window_size: int = _WINDOW_SIZE):
        self.model_id = model_id
        self.window_size = window_size

        # Sliding windows — one per pipeline stage
        self._preprocessing_time_window = deque(maxlen=window_size)   # VLM only (ms)
        self._ttft_window = deque(maxlen=window_size)                  # Time to First Token (ms)
        self._tokens_per_second_window = deque(maxlen=window_size)     # Tokens/sec
        self._stream_latency_window = deque(maxlen=window_size)        # Avg inter-token latency (ms)
        self._total_pipeline_latency_window = deque(maxlen=window_size) # End-to-end (ms)

        self._total_requests = 0
        self._total_failures = 0
        self._consecutive_failures = 0
        self._lock = threading.Lock()

    def record_request(
        self,
        total_pipeline_latency_ms: float,
        tokens_generated: int,
        ttft_ms: Optional[float] = None,
        token_generation_time_ms: Optional[float] = None,
        avg_stream_latency_ms: Optional[float] = None,
        preprocessing_time_ms: Optional[float] = None,
    ):
        """
        Record metrics for one completed (successful) request.
        Resets consecutive_failures on success.

        Args:
            total_pipeline_latency_ms : End-to-end latency in ms
            tokens_generated          : Number of tokens generated
            ttft_ms                   : Time to first token in ms (from inference start)
            token_generation_time_ms  : Pure token generation time (first to last token) in ms
            avg_stream_latency_ms     : Average inter-token latency in ms
            preprocessing_time_ms     : Image encode/resize time in ms (VLM only)
        """
        with self._lock:
            # Tokens/sec calculation - use pure generation time for accuracy
            if tokens_generated > 1:  # Need at least 2 tokens for meaningful TPS
                if token_generation_time_ms and token_generation_time_ms > 0:
                    # Best: use pure generation time (first token to last token)
                    # Subtract 1 from tokens_generated because TTFT measures the first token
                    tps = ((tokens_generated - 1) / token_generation_time_ms) * 1000.0
                    self._tokens_per_second_window.append(tps)
                elif ttft_ms is not None and total_pipeline_latency_ms > ttft_ms:
                    # Fallback: exclude TTFT from total pipeline time
                    steady_state_time = total_pipeline_latency_ms - ttft_ms
                    if steady_state_time > 0:
                        tps = ((tokens_generated - 1) / steady_state_time) * 1000.0
                        self._tokens_per_second_window.append(tps)
                elif total_pipeline_latency_ms > 0:
                    # Last resort: use total pipeline time (less accurate)
                    tps = (tokens_generated / total_pipeline_latency_ms) * 1000.0
                    self._tokens_per_second_window.append(tps)

            if ttft_ms is not None:
                self._ttft_window.append(ttft_ms)

            if avg_stream_latency_ms is not None:
                self._stream_latency_window.append(avg_stream_latency_ms)

            self._total_pipeline_latency_window.append(total_pipeline_latency_ms)

            if preprocessing_time_ms is not None:
                self._preprocessing_time_window.append(preprocessing_time_ms)

            self._total_requests += 1
            # Reset consecutive failures on success
            self._consecutive_failures = 0
            logger.info(
                f"Model {self.model_id}: request recorded — "
                f"total_requests={self._total_requests}, "
                f"total_failures={self._total_failures}, "
                f"consecutive_failures={self._consecutive_failures}"
            )

    def record_failure(self):
        """
        Record an inference failure for this model.
        Increments consecutive_failures, total_failures, and total_requests.
        consecutive_failures is reset to 0 on the next successful request.
        """
        with self._lock:
            self._consecutive_failures += 1
            self._total_failures += 1
            self._total_requests += 1
            logger.info(
                f"Model {self.model_id}: failure recorded — "
                f"total_requests={self._total_requests}, "
                f"total_failures={self._total_failures}, "
                f"consecutive_failures={self._consecutive_failures}"
            )

    def get_averages(self) -> Dict:
        """
        Return moving averages for all tracked pipeline stages.

        Returns:
            Dict with avg_* keys; None values for stages not yet observed.
        """
        with self._lock:
            def _avg(window: deque) -> Optional[float]:
                return round(sum(window) / len(window), 3) if window else None

            result = {
                "avg_preprocessing_time_ms": _avg(self._preprocessing_time_window),
                "avg_ttft_ms": _avg(self._ttft_window),
                "avg_tokens_per_second": _avg(self._tokens_per_second_window),
                "avg_stream_latency_ms": _avg(self._stream_latency_window),
                "avg_total_pipeline_latency_ms": _avg(self._total_pipeline_latency_window),
                "consecutive_failures": self._consecutive_failures,
            }
            return result


class MetricsManager:
    """
    Singleton manager for system and model inference metrics.

    Exposes a single get_metrics() method that returns a unified snapshot
    combining live system resource stats with per-model moving averages.
    """

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._model_metrics: Dict[str, ModelMetrics] = {}
        self._metrics_lock = threading.Lock()
        logger.info("MetricsManager initialized")

    @classmethod
    def get_instance(cls) -> "MetricsManager":
        """Return the singleton instance."""
        return cls()

    # ------------------------------------------------------------------
    # Recording
    # ------------------------------------------------------------------

    def record_inference_failure(self, model_id: str):
        """
        Record an inference failure for a model (e.g. subprocess crash, SDK error).
        Does NOT record cancelled requests — only genuine execution failures.

        Args:
            model_id: Model identifier
        """
        model_metrics = self._get_or_create(model_id)
        model_metrics.record_failure()

    def record_inference_metrics(
        self,
        model_id: str,
        total_pipeline_latency_ms: float,
        tokens_generated: int,
        ttft_ms: Optional[float] = None,
        token_generation_time_ms: Optional[float] = None,
        avg_stream_latency_ms: Optional[float] = None,
        preprocessing_time_ms: Optional[float] = None,
    ):
        """
        Record inference metrics for a completed request.

        Args:
            model_id                  : Model identifier
            total_pipeline_latency_ms : End-to-end latency in ms
            tokens_generated          : Number of tokens generated
            ttft_ms                   : Time to first token in ms (from inference start)
            token_generation_time_ms  : Pure token generation time (first to last token) in ms
            avg_stream_latency_ms     : Average inter-token latency in ms
            preprocessing_time_ms     : Image encode/resize time in ms (VLM only)
        """
        model_metrics = self._get_or_create(model_id)
        model_metrics.record_request(
            total_pipeline_latency_ms=total_pipeline_latency_ms,
            tokens_generated=tokens_generated,
            ttft_ms=ttft_ms,
            token_generation_time_ms=token_generation_time_ms,
            avg_stream_latency_ms=avg_stream_latency_ms,
            preprocessing_time_ms=preprocessing_time_ms,
        )

    # ------------------------------------------------------------------
    # Querying
    # ------------------------------------------------------------------

    def get_metrics(self) -> Dict:
        """
        Return a unified metrics snapshot:
          - system: live CPU and memory stats
          - models: per-model moving averages

        Returns:
            Dict with 'system' and 'models' keys
        """
        return {
            "system": self._get_system_metrics(),
            "models": self._get_all_model_averages(),
        }

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _get_or_create(self, model_id: str) -> ModelMetrics:
        with self._metrics_lock:
            if model_id not in self._model_metrics:
                self._model_metrics[model_id] = ModelMetrics(model_id)
                logger.info(f"MetricsManager: created tracker for model '{model_id}'")
            return self._model_metrics[model_id]

    def _get_all_model_averages(self) -> Dict[str, Dict]:
        with self._metrics_lock:
            return {
                model_id: metrics.get_averages()
                for model_id, metrics in self._model_metrics.items()
            }

    def _get_system_metrics(self) -> Dict:
        try:
            cpu_percent = psutil.cpu_percent(interval=0.1)
            mem = psutil.virtual_memory()
            return {
                "cpu_usage_percent": round(cpu_percent, 2),
                "memory_total_gb": round(mem.total / (1024 ** 3), 2),
                "memory_used_gb": round(mem.used / (1024 ** 3), 2),
                "memory_free_gb": round(mem.available / (1024 ** 3), 2),
                "memory_usage_percent": round(mem.percent, 2),
            }
        except Exception as e:
            logger.error(f"MetricsManager: error reading system metrics: {e}", exc_info=True)
            return {
                "cpu_usage_percent": 0.0,
                "memory_total_gb": 0.0,
                "memory_used_gb": 0.0,
                "memory_free_gb": 0.0,
                "memory_usage_percent": 0.0,
                "error": str(e),
            }
