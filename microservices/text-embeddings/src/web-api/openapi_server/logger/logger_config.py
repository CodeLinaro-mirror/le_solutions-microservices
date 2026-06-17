# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Application-wide logging configuration for the openapi_server package.

Writes structured log output to **both** stdout (for Docker/container log
aggregation) and a rotating file at ``/tmp/openapi_server.log`` (for
persistent, post-mortem analysis inside the container).

Configuration is driven by environment variables so that no code changes
are needed to adjust log verbosity or file location at deployment time:

``OPENAPI_LOG_FILE``
    Path to the persistent log file.
    Default: ``/tmp/openapi_server.log``

``OPENAPI_LOG_LEVEL``
    Minimum log level for both handlers.
    One of ``DEBUG``, ``INFO`` (default), ``WARNING``, ``ERROR``, ``CRITICAL``.
    Case-insensitive.

``OPENAPI_LOG_MAX_BYTES``
    Maximum size of a single log file before rotation (bytes).
    Default: ``10485760`` (10 MiB).

``OPENAPI_LOG_BACKUP_COUNT``
    Number of rotated backup files to keep alongside the active log.
    Default: ``5``.

``OPENAPI_LOG_DISABLE_FILE``
    Set to ``1`` to suppress file logging entirely (stdout only).
    Default: ``0``.
"""

from __future__ import annotations

import logging
import logging.handlers
import os
import sys
import threading

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_DEFAULT_LOG_FILE     = "/tmp/openapi_server.log"
_DEFAULT_LOG_LEVEL    = "INFO"
_DEFAULT_MAX_BYTES    = 10 * 1024 * 1024   # 10 MiB
_DEFAULT_BACKUP_COUNT = 5

# Shared format used by both the stdout and file handlers so that log lines
# look identical regardless of where they are read.
_LOG_FORMAT  = "%(asctime)s - %(levelname)s - %(name)s - %(message)s"
_DATE_FORMAT = "%Y-%m-%d %H:%M:%S"

# ---------------------------------------------------------------------------
# Thread-safe initialisation guard
# ---------------------------------------------------------------------------

_init_lock   = threading.Lock()


class LoggerConfig:
    """
    Centralised logging configurator for the openapi_server package.

    Usage
    -----
    Call ``LoggerConfig.initialize()`` once at application startup (already
    done in ``main.py``).  Everywhere else, obtain a logger with::

        logger = LoggerConfig.get_logger(__name__)

    The logger writes to:

    * **stdout** — captured by Docker / container log aggregators.
    * **/tmp/openapi_server.log** — persistent rotating file inside the
      container, readable with ``docker exec … tail -f /tmp/openapi_server.log``
      or mounted as a volume for external log shipping.
    """

    _initialized: bool = False

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    @classmethod
    def initialize(cls, level: int = logging.INFO) -> None:
        """
        Configure the root logger exactly once.

        Subsequent calls are no-ops (guarded by ``_initialized``).

        Parameters
        ----------
        level:
            Fallback log level used when ``OPENAPI_LOG_LEVEL`` is not set.
            Accepts a ``logging`` integer constant (e.g. ``logging.DEBUG``).
        """
        with _init_lock:
            if cls._initialized:
                return

            # ── Resolve configuration from environment ────────────────────
            log_file     = os.environ.get("OPENAPI_LOG_FILE",     _DEFAULT_LOG_FILE)
            level_str    = os.environ.get("OPENAPI_LOG_LEVEL",    _DEFAULT_LOG_LEVEL)
            max_bytes    = int(os.environ.get("OPENAPI_LOG_MAX_BYTES",    _DEFAULT_MAX_BYTES))
            backup_count = int(os.environ.get("OPENAPI_LOG_BACKUP_COUNT", _DEFAULT_BACKUP_COUNT))
            disable_file = os.environ.get("OPENAPI_LOG_DISABLE_FILE", "0").strip() in ("1", "true", "yes")

            # Environment variable takes precedence over the caller's default.
            effective_level = cls._resolve_level(level_str) if level_str else level

            # ── Formatter (shared by all handlers) ────────────────────────
            formatter = logging.Formatter(fmt=_LOG_FORMAT, datefmt=_DATE_FORMAT)

            # ── Root logger ───────────────────────────────────────────────
            root = logging.getLogger()
            root.setLevel(effective_level)

            # Remove any handlers added by a previous basicConfig call or
            # by third-party libraries that import before us.
            root.handlers.clear()

            # ── Handler 1: stdout (always present) ───────────────────────
            stdout_handler = logging.StreamHandler(sys.stdout)
            stdout_handler.setLevel(effective_level)
            stdout_handler.setFormatter(formatter)
            root.addHandler(stdout_handler)

            # ── Handler 2: rotating file ──────────────────────────────────
            if not disable_file:
                file_handler = cls._make_file_handler(
                    log_file, max_bytes, backup_count, effective_level, formatter
                )
                if file_handler is not None:
                    root.addHandler(file_handler)

            cls._initialized = True

            # Emit a startup banner so the log file has a clear session
            # boundary and the configuration is visible in both outputs.
            startup_logger = logging.getLogger(__name__)
            startup_logger.info(
                "LoggerConfig initialised — "
                "level=%s file=%r max_bytes=%d backup_count=%d file_disabled=%s",
                logging.getLevelName(effective_level),
                log_file, max_bytes, backup_count, disable_file,
            )

    @staticmethod
    def get_logger(name: str) -> logging.Logger:
        """
        Return a :class:`logging.Logger` for *name*.

        Parameters
        ----------
        name:
            Typically ``__name__`` of the calling module.

        Returns
        -------
        logging.Logger
            A logger that inherits handlers from the root logger configured
            by :meth:`initialize`.
        """
        return logging.getLogger(name)

    # ------------------------------------------------------------------
    # Private helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _resolve_level(level_str: str) -> int:
        """Convert a level name string to a ``logging`` integer constant."""
        mapping = {
            "debug":    logging.DEBUG,
            "info":     logging.INFO,
            "warning":  logging.WARNING,
            "warn":     logging.WARNING,
            "error":    logging.ERROR,
            "critical": logging.CRITICAL,
        }
        return mapping.get(level_str.lower().strip(), logging.INFO)

    @staticmethod
    def _make_file_handler(
        log_file: str,
        max_bytes: int,
        backup_count: int,
        level: int,
        formatter: logging.Formatter,
    ) -> logging.Handler | None:
        """
        Create a ``RotatingFileHandler`` for *log_file*.

        Returns ``None`` if the file cannot be opened (e.g. permission
        denied inside a read-only container layer), so the caller can
        continue with stdout-only logging rather than crashing at startup.
        """
        try:
            # Ensure the parent directory exists.  /tmp always exists in a
            # Linux container, but a custom path might not.
            parent = os.path.dirname(os.path.abspath(log_file))
            os.makedirs(parent, exist_ok=True)

            handler = logging.handlers.RotatingFileHandler(
                filename=log_file,
                mode="a",               # append across container restarts
                maxBytes=max_bytes,
                backupCount=backup_count,
                encoding="utf-8",
                delay=False,            # create the file immediately at startup
            )
            handler.setLevel(level)
            handler.setFormatter(formatter)

            # Suppress the default error-raising behaviour so that a
            # transient write failure (disk full, NFS hiccup) does not
            # propagate into the request handler and return a 500 to the
            # client.
            handler.handleError = lambda record: None  # type: ignore[method-assign]

            return handler

        except OSError as exc:
            # Non-fatal: warn on stderr and continue without file logging.
            print(
                f"[LoggerConfig] WARNING: Cannot open log file '{log_file}': "
                f"{exc}. File logging disabled.",
                file=sys.stderr,
                flush=True,
            )
            return None
