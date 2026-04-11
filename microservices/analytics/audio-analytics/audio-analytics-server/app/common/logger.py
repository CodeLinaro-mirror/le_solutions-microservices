# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import logging
import sys
from typing import Optional
from .config import Config


def setup_logger(name: str, level: Optional[int] = None) -> logging.Logger:
    """
    Setup a logger with consistent formatting.
    
    Args:
        name: Logger name (typically __name__ or service name)
        level: Optional log level override
        
    Returns:
        Configured logger instance
    """
    logger = logging.getLogger(name)
    
    # Set level
    log_level = level if level is not None else Config.LOG_LEVEL
    logger.setLevel(log_level)
    # Prevent log propagation to parent to avoid duplicate logs
    logger.propagate = False
    
    # Avoid duplicate handlers
    if logger.handlers:
        return logger
    
    # Create console handler
    handler = logging.StreamHandler(sys.stdout)
    handler.setLevel(log_level)
    
    # Create formatter
    formatter = logging.Formatter(
        '[server] %(asctime)s %(levelname)s [%(name)s] %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    handler.setFormatter(formatter)
    
    # Add handler to logger
    logger.addHandler(handler)
    
    return logger


def get_logger(name: str) -> logging.Logger:
    """
    Get or create a logger with the given name.
    
    Args:
        name: Logger name
        
    Returns:
        Logger instance
    """
    return setup_logger(name)
