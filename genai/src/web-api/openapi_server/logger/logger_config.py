# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import logging
import os

class LoggerConfig:
    _initialized = False  # Class-level flag to prevent re-initialization

    @classmethod
    def initialize(cls, level=None):
        if not cls._initialized:
            if level is None:
                # Read from environment variable, default to ERROR for production
                log_level_str = os.getenv('LOG_LEVEL', 'ERROR').upper()
                level = getattr(logging, log_level_str, logging.ERROR)

            logging.basicConfig(
                level=level,
                format='%(asctime)s - %(levelname)s - %(name)s - %(message)s'
            )
            cls._initialized = True  # Mark as initialized

    @staticmethod
    def get_logger(name: str):
        return logging.getLogger(name)
