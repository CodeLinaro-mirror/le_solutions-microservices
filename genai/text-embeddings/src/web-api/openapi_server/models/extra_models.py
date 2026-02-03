# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from pydantic import BaseModel

class TokenModel(BaseModel):
    """Defines a token model."""

    sub: str
