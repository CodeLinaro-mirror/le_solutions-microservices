# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Dict, List, Tuple  # noqa: F401

from typing import List
from openapi_server.models.usage_read import UsageRead


class BaseHealthApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BaseHealthApi.subclasses = BaseHealthApi.subclasses + (cls,)
    async def healthcheck(
        self,
    ) -> List[UsageRead]:
        """Health Check of various dependencies"""
        ...
