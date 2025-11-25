# ComputerToolCallSafetyCheck

A pending safety check for the computer call. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the pending safety check. | 
**code** | **str** | The type of the pending safety check. | 
**message** | **str** | Details about the pending safety check. | 

## Example

```python
from openapi_client.models.computer_tool_call_safety_check import ComputerToolCallSafetyCheck

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerToolCallSafetyCheck from a JSON string
computer_tool_call_safety_check_instance = ComputerToolCallSafetyCheck.from_json(json)
# print the JSON string representation of the object
print(ComputerToolCallSafetyCheck.to_json())

# convert the object into a dict
computer_tool_call_safety_check_dict = computer_tool_call_safety_check_instance.to_dict()
# create an instance of ComputerToolCallSafetyCheck from a dict
computer_tool_call_safety_check_from_dict = ComputerToolCallSafetyCheck.from_dict(computer_tool_call_safety_check_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


