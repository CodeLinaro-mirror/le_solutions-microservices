# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
LiteRT backend logging subsystem.

Provides a thread-safe, file-backed logger for the LiteRT / QNN HTP
inference pipeline.  All log output is written to ``/tmp/litert_backend.log``
(configurable via the ``LITERT_LOG_FILE`` environment variable) using a
rotating file handler so that the log file never grows unbounded.

Quick start
-----------
::

    from openapi_server.impl.litert_backend.logger import get_logger

    log = get_logger(__name__)
    log.info("Model loaded successfully")
    log.debug("Input tensor shape: %s", ids.shape)
    log.error("LiteRtCreateCompiledModel failed: %s", status)

Environment variables
---------------------
``LITERT_LOG_FILE``
    Path to the log file.  Default: ``/tmp/litert_backend.log``.

``LITERT_LOG_LEVEL``
    Minimum log level written to the file.  One of ``DEBUG``, ``INFO``
    (default), ``WARNING``, ``ERROR``, ``CRITICAL``.  Case-insensitive.

``LITERT_LOG_MAX_BYTES``
    Maximum size of a single log file before rotation (bytes).
    Default: ``10485760`` (10 MiB).

``LITERT_LOG_BACKUP_COUNT``
    Number of rotated backup files to keep.  Default: ``5``.

``LITERT_LOG_CONSOLE``
    Set to ``1`` to also echo log output to stderr.  Default: ``0``.
"""

from __future__ import annotations

import logging
import logging.handlers
import os
import sys
import threading
from typing import Optional

# ---------------------------------------------------------------------------
# Module-level constants (overridable via environment variables)
# ---------------------------------------------------------------------------

_DEFAULT_LOG_FILE     = "/tmp/litert_backend.log"
_DEFAULT_LOG_LEVEL    = "INFO"
_DEFAULT_MAX_BYTES    = 10 * 1024 * 1024   # 10 MiB
_DEFAULT_BACKUP_COUNT = 5

# Name of the root logger for the entire LiteRT backend subsystem.
# All child loggers (get_logger(__name__)) inherit its handlers.
_ROOT_LOGGER_NAME = "litert_backend"

# ---------------------------------------------------------------------------
# Log record formatter
# ---------------------------------------------------------------------------

_LOG_FORMAT = (
    "%(asctime)s.%(msecs)03d "
    "[%(levelname)-8s] "
    "%(name)s "
    "(%(funcName)s:%(lineno)d) "
    "pid=%(process)d tid=%(thread)d "
    "— %(message)s"
)
_DATE_FORMAT = "%Y-%m-%dT%H:%M:%S"

# ---------------------------------------------------------------------------
# Initialisation guard
# ---------------------------------------------------------------------------

_init_lock   = threading.Lock()
_initialised = False


def _resolve_log_level(level_str: str) -> int:
    """Convert a level name string to a ``logging`` integer constant."""
    level_map = {
        "debug":    logging.DEBUG,
        "info":     logging.INFO,
        "warning":  logging.WARNING,
        "warn":     logging.WARNING,
        "error":    logging.ERROR,
        "critical": logging.CRITICAL,
    }
    return level_map.get(level_str.lower().strip(), logging.INFO)


def _make_file_handler(log_file: str, max_bytes: int, backup_count: int) -> Optional[logging.Handler]:
    """
    Create a ``RotatingFileHandler`` for *log_file*.

    Returns ``None`` if the file cannot be opened (e.g. permission denied),
    so the caller can fall back to stderr without crashing the backend.
    """
    try:
        # Ensure the parent directory exists (it always does for /tmp, but
        # a custom path might not).
        parent = os.path.dirname(os.path.abspath(log_file))
        os.makedirs(parent, exist_ok=True)

        handler = logging.handlers.RotatingFileHandler(
            filename=log_file,
            mode="a",                   # append across sessions
            maxBytes=max_bytes,
            backupCount=backup_count,
            encoding="utf-8",
            delay=False,                # create the file immediately
        )
        return handler
    except OSError as exc:
        # Non-fatal: log to stderr and continue without a file handler.
        print(
            f"[litert_backend.logger] WARNING: Cannot open log file "
            f"'{log_file}': {exc}.  File logging disabled.",
            file=sys.stderr,
            flush=True,
        )
        return None


def _initialise() -> None:
    """
    Configure the root ``litert_backend`` logger exactly once.

    Subsequent calls are no-ops (guarded by ``_initialised``).
    This function is called automatically the first time ``get_logger``
    is invoked, so callers never need to call it explicitly.
    """
    global _initialised

    with _init_lock:
        if _initialised:
            return

        # ── Read configuration from environment ───────────────────────────
        log_file     = os.environ.get("LITERT_LOG_FILE",     _DEFAULT_LOG_FILE)
        level_str    = os.environ.get("LITERT_LOG_LEVEL",    _DEFAULT_LOG_LEVEL)
        max_bytes    = int(os.environ.get("LITERT_LOG_MAX_BYTES",    _DEFAULT_MAX_BYTES))
        backup_count = int(os.environ.get("LITERT_LOG_BACKUP_COUNT", _DEFAULT_BACKUP_COUNT))
        echo_console = os.environ.get("LITERT_LOG_CONSOLE", "0").strip() in ("1", "true", "yes")

        level = _resolve_log_level(level_str)

        # ── Build formatter ───────────────────────────────────────────────
        formatter = logging.Formatter(fmt=_LOG_FORMAT, datefmt=_DATE_FORMAT)

        # ── Configure root litert_backend logger ──────────────────────────
        root = logging.getLogger(_ROOT_LOGGER_NAME)
        root.setLevel(level)
        root.propagate = False   # do not bubble up to the application root logger

        # Remove any handlers that may have been added by a previous (failed)
        # initialisation attempt.
        root.handlers.clear()

        # ── File handler (primary) ────────────────────────────────────────
        file_handler = _make_file_handler(log_file, max_bytes, backup_count)
        if file_handler is not None:
            file_handler.setLevel(level)
            file_handler.setFormatter(formatter)
            # Disable the default error-raising behaviour so that a transient
            # write failure (e.g. disk full) does not crash the inference loop.
            file_handler.handleError = lambda record: None  # type: ignore[method-assign]
            root.addHandler(file_handler)

        # ── Optional console handler (stderr) ─────────────────────────────
        if echo_console or file_handler is None:
            console_handler = logging.StreamHandler(sys.stderr)
            console_handler.setLevel(level)
            console_handler.setFormatter(formatter)
            root.addHandler(console_handler)

        _initialised = True

        # Emit a startup banner so the log file has a clear session boundary.
        root.info(
            "LiteRT backend logger initialised — "
            "log_file=%r level=%s max_bytes=%d backup_count=%d",
            log_file, level_str.upper(), max_bytes, backup_count,
        )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def get_logger(name: str) -> logging.Logger:
    """
    Return a :class:`logging.Logger` scoped to *name* under the
    ``litert_backend`` hierarchy.

    The logger is automatically initialised on first call.

    Parameters
    ----------
    name:
        Typically ``__name__`` of the calling module, e.g.
        ``"openapi_server.impl.litert_backend.backend"``.
        The ``litert_backend.`` prefix is prepended automatically if
        *name* does not already start with it.

    Returns
    -------
    logging.Logger
        A fully configured, thread-safe logger instance.

    Examples
    --------
    ::

        log = get_logger(__name__)
        log.info("Interpreter created for model %r", model_path)
        log.debug("Tensor buffer size: %d bytes", buf_sz)
        log.warning("HTP compilation failed; falling back to CPU")
        log.error("LiteRtRunCompiledModel failed: %s", status_str)
    """
    _initialise()

    # Ensure the logger lives under the litert_backend hierarchy so it
    # inherits the file handler configured above.
    if not name.startswith(_ROOT_LOGGER_NAME):
        qualified = f"{_ROOT_LOGGER_NAME}.{name}"
    else:
        qualified = name

    return logging.getLogger(qualified)


def set_level(level: str) -> None:
    """
    Change the effective log level at runtime.

    Parameters
    ----------
    level:
        One of ``"DEBUG"``, ``"INFO"``, ``"WARNING"``, ``"ERROR"``,
        ``"CRITICAL"`` (case-insensitive).

    Examples
    --------
    ::

        from openapi_server.impl.litert_backend.logger import set_level
        set_level("DEBUG")   # enable verbose output for a debugging session
    """
    _initialise()
    root = logging.getLogger(_ROOT_LOGGER_NAME)
    int_level = _resolve_log_level(level)
    root.setLevel(int_level)
    for handler in root.handlers:
        handler.setLevel(int_level)
    root.info("Log level changed to %s", level.upper())


def get_log_file_path() -> str:
    """Return the configured log file path (resolved from environment)."""
    return os.environ.get("LITERT_LOG_FILE", _DEFAULT_LOG_FILE)
