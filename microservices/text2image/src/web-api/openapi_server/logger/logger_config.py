# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import logging

class LoggerConfig:
    _initialized = False  # Class-level flag to prevent re-initialization

    @classmethod
    def initialize(cls, level=logging.INFO):
        if not cls._initialized:
            logging.basicConfig(
                level=level,
                format='%(asctime)s - %(levelname)s - %(name)s - %(message)s'
            )
            cls._initialized = True  # Mark as initialized

    @staticmethod
    def get_logger(name: str):
        return logging.getLogger(name)
    