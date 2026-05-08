# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Health API Implementation

Implements a real health check for Docker Swarm / Kubernetes orchestration.

Health is determined by two criteria:
1. Consecutive inference failures per model — if any model has >= 3 consecutive
   failures it indicates a lower-level system fault (DSP hang, FastRPC session
   collapse, etc.) that cannot self-recover.
2. Critical memory pressure — if available system memory drops below 100 MB the
   container is considered unhealthy.

Returns:
  HTTP 200  {"status": "healthy",   ...}
  HTTP 503  {"status": "unhealthy", "reason": "Server unavailable due to critical system resource unavailability"}
"""

from openapi_server.apis.health_api_base import BaseHealthApi
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes

from fastapi import Response
from fastapi.responses import JSONResponse

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

# Threshold: 3 consecutive failures for the same model → unhealthy
_CONSECUTIVE_FAILURE_THRESHOLD = 3

# Threshold: less than 100 MB free → unhealthy
_CRITICAL_MEMORY_FREE_MB = 100

_UNHEALTHY_REASON = "Server unavailable due to critical system resource unavailability"


class HealthApiImpl(BaseHealthApi):
    async def healthcheck(self):
        """
        Real health check for container orchestration.

        Checks:
        - Consecutive inference failures per model (DSP/FastRPC fault detection)
        - Available system memory (critical memory pressure)

        Returns HTTP 200 when healthy, HTTP 503 when unhealthy.
        """
        try:
            from openapi_server.managers.metrics_manager import MetricsManager
            from openapi_server.managers.system_resource_manager import SystemResourceManager

            metrics_manager = MetricsManager.get_instance()
            resource_manager = SystemResourceManager()

            # ── 1. Check consecutive inference failures ──────────────────────
            model_metrics = metrics_manager.get_metrics().get("models", {})
            for model_id, stats in model_metrics.items():
                consecutive = stats.get("consecutive_failures", 0)
                if consecutive >= _CONSECUTIVE_FAILURE_THRESHOLD:
                    logger.warning(
                        f"Health check FAILED: model '{model_id}' has "
                        f"{consecutive} consecutive failures (threshold={_CONSECUTIVE_FAILURE_THRESHOLD}). "
                        f"Possible DSP/FastRPC fault."
                    )
                    return JSONResponse(
                        status_code=HttpStatusCodes.SERVICE_UNAVAILABLE,
                        content={
                            "status": "unhealthy",
                            "reason": _UNHEALTHY_REASON,
                        }
                    )

            # ── 2. Check available memory ────────────────────────────────────
            available_mb = resource_manager.get_available_memory_mb()
            if available_mb < _CRITICAL_MEMORY_FREE_MB:
                logger.warning(
                    f"Health check FAILED: only {available_mb} MB available "
                    f"(threshold={_CRITICAL_MEMORY_FREE_MB} MB)."
                )
                return JSONResponse(
                    status_code=HttpStatusCodes.SERVICE_UNAVAILABLE,
                    content={
                        "status": "unhealthy",
                        "reason": _UNHEALTHY_REASON,
                    }
                )

            # ── All checks passed ────────────────────────────────────────────
            logger.debug("Health check passed")
            return JSONResponse(
                status_code=HttpStatusCodes.OK,
                content={"status": "healthy"}
            )

        except Exception as e:
            logger.error(f"Unexpected error in healthcheck: {e}", exc_info=True)
            return JSONResponse(
                status_code=HttpStatusCodes.SERVICE_UNAVAILABLE,
                content={
                    "status": "unhealthy",
                    "reason": _UNHEALTHY_REASON,
                }
            )
