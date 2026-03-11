# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations
import json
import pprint
from pydantic import BaseModel, Field, StrictBool, StrictStr, field_validator
from typing import Any, ClassVar, Dict, List, Optional
try:
    from typing import Self
except ImportError:
    from typing_extensions import Self


class ChatCompletionCancelled(BaseModel):
    """Represents the cancellation confirmation of an active chat completion."""

    object: StrictStr = Field(description="The object type, always 'chat.completion.cancelled'.")
    id: StrictStr = Field(description="The ID of the chat completion that was cancelled.")
    cancelled: StrictBool = Field(description="Whether the chat completion was successfully cancelled.")
    message: Optional[StrictStr] = Field(default=None, description="Additional information about the cancellation.")

    __properties: ClassVar[List[str]] = ["object", "id", "cancelled", "message"]

    @field_validator('object')
    def object_validate_enum(cls, value):
        if value not in ('chat.completion.cancelled',):
            raise ValueError("must be one of enum values ('chat.completion.cancelled')")
        return value

    model_config = {
        "populate_by_name": True,
        "validate_assignment": True,
        "protected_namespaces": (),
    }

    def to_str(self) -> str:
        return pprint.pformat(self.model_dump(by_alias=True))

    def to_json(self) -> str:
        return json.dumps(self.to_dict())

    @classmethod
    def from_json(cls, json_str: str) -> Self:
        return cls.from_dict(json.loads(json_str))

    def to_dict(self) -> Dict[str, Any]:
        return self.model_dump(by_alias=True, exclude={}, exclude_none=True)

    @classmethod
    def from_dict(cls, obj: Dict) -> Self:
        if obj is None:
            return None
        if not isinstance(obj, dict):
            return cls.model_validate(obj)
        return cls.model_validate({
            "object": obj.get("object"),
            "id": obj.get("id"),
            "cancelled": obj.get("cancelled"),
            "message": obj.get("message")
        })
