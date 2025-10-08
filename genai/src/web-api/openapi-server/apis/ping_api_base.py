# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Dict, List, Tuple  # noqa: F401

from openapi_server.models.ping_response import PingResponse


class BasePingApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BasePingApi.subclasses = BasePingApi.subclasses + (cls,)
    async def ping(
        self,
    ) -> PingResponse:
        """Ping the server to check it&#39;s alive and well. Ping will return Pong if you provide the correct BearerToken"""
        ...
