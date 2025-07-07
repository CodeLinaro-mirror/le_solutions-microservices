# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap
from test_constant import HandleObjectInterfaceConstant as HANDLE_OBJ_CONST
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
import pytest

def test_handle_object_interface(client: TestClient):
   """
   Test case for handle object interface
   """
   # Check singleton
   map1 = HandleIdObjectMap()
   map2 = HandleIdObjectMap()
   assert map1 == map2

   # clear mapping
   map1.clear_mapping()

   # Set and get handle
   map1.set_handle(HANDLE_OBJ_CONST.VALUE_1, HANDLE_OBJ_CONST.ID_1)
   assert map1.get_handle(HANDLE_OBJ_CONST.ID_1) == HANDLE_OBJ_CONST.VALUE_1

   # Update existing handle
   map1.set_handle(HANDLE_OBJ_CONST.VALUE_2, HANDLE_OBJ_CONST.ID_1)
   assert map1.get_handle(HANDLE_OBJ_CONST.ID_1) == HANDLE_OBJ_CONST.VALUE_2

   # Delete handle
   assert map1.delete_handle(HANDLE_OBJ_CONST.ID_1) == True

   # Get deleted handle
   assert map1.delete_handle(HANDLE_OBJ_CONST.ID_1) == False

   # Get mapping and size
   map1.clear_mapping()
   assert map1.get_current_size() == HANDLE_OBJ_CONST.CURRENT_SIZE_COUNT_ZERO
   map1.set_handle(HANDLE_OBJ_CONST.VALUE_1, HANDLE_OBJ_CONST.ID_1)
   map1.set_handle(HANDLE_OBJ_CONST.VALUE_2, HANDLE_OBJ_CONST.ID_2)
   mapping = map1.get_mapping()
   assert len(mapping) == HANDLE_OBJ_CONST.MAPPING_LEN_TWO
   assert map1.get_current_size() == HANDLE_OBJ_CONST.CURRENT_SIZE_COUNT_TWO

   # Check Rollover
   llm_service = LLMService()
   model_str = HANDLE_OBJ_CONST.MODEL
   model_input = llm_service.ffi.new("char[]", model_str.encode('utf-8'))
   map1.clear_mapping()
   map1.MAX_ELEMENTS = HANDLE_OBJ_CONST.MAX_ELEMENT_COUNT_FOUR
   for i in range(map1.MAX_ELEMENTS + 1):
      handle = llm_service.lib.llm_create_object(model_input)
      map1.set_handle(handle, f"id{i}")
   assert map1.get_current_size() ==  map1.MAX_ELEMENTS
   assert map1.get_handle(HANDLE_OBJ_CONST.ID_0) == None
   map1.MAX_ELEMENTS = HANDLE_OBJ_CONST.MAX_ELEMENT_COUNT_FIFTY

   # ID length is greater than MAX SIZE
   id = 'a' * (map1.MAX_ID_LENGTH + 1)
   with pytest.raises(ValueError):
      map1.set_handle(HANDLE_OBJ_CONST.VALUE_1, id)

