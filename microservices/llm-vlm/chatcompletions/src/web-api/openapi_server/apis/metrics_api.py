# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Metrics API

Single /v1/metrics endpoint returning:
  - Live system resource stats (CPU, memory)
  - Per-model moving averages for all pipeline stages
"""

from fastapi import APIRouter, HTTPException
from typing import Dict

from openapi_server.managers.metrics_manager import MetricsManager
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

router = APIRouter()


@router.get(
    "/v1/metrics",
    responses={
        200: {
            "description": "System and model inference metrics",
            "content": {
                "application/json": {
                    "example": {
                        "system": {
                            "cpu_usage_percent": 45.2,
                            "memory_total_gb": 11.0,
                            "memory_used_gb": 4.1,
                            "memory_free_gb": 6.9,
                            "memory_usage_percent": 37.3
                        },
                        "models": {
                            "qwen2.5-7b": {
                                "avg_preprocessing_time_ms": None,
                                "avg_ttft_ms": 120.5,
                                "avg_tokens_per_second": 25.4,
                                "avg_stream_latency_ms": 39.4,
                                "avg_total_pipeline_latency_ms": 4500.0
                            },
                            "llava-v1.5-7b": {
                                "avg_preprocessing_time_ms": 18.3,
                                "avg_ttft_ms": 210.7,
                                "avg_tokens_per_second": 12.1,
                                "avg_stream_latency_ms": 82.6,
                                "avg_total_pipeline_latency_ms": 6200.0
                            }
                        }
                    }
                }
            }
        }
    },
    tags=["metrics"],
    summary="Get system and model inference metrics",
    response_model_by_alias=True,
)
async def get_metrics() -> Dict:
    """
    Get a unified metrics snapshot.

    **System metrics** (live):
    - `cpu_usage_percent` — current CPU utilization
    - `memory_total_gb` / `memory_used_gb` / `memory_free_gb` — absolute memory stats
    - `memory_usage_percent` — memory utilization

    **Per-model moving averages** (last 100 requests):
    - `avg_preprocessing_time_ms` — image encode/resize time (VLM only; null for LLM)
    - `avg_ttft_ms` — Time to First Token
    - `avg_tokens_per_second` — steady-state token generation rate
    - `avg_stream_latency_ms` — average inter-token stream latency
    - `avg_total_pipeline_latency_ms` — end-to-end total pipeline latency
    """
    try:
        metrics = MetricsManager.get_instance().get_metrics()
        for model_stats in metrics.get("models", {}).values():
            model_stats.pop("consecutive_failures", None)
        logger.debug("Metrics snapshot retrieved")
        return metrics
    except Exception as e:
        logger.error(f"Error retrieving metrics: {e}", exc_info=True)
        raise HTTPException(status_code=500, detail=f"Failed to retrieve metrics: {str(e)}")
