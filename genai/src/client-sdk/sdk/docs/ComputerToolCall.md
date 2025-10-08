# ComputerToolCall

A tool call to a computer use tool. See the  [computer use guide](/docs/guides/tools-computer-use) for more information. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the computer call. Always &#x60;computer_call&#x60;. | [default to 'computer_call']
**id** | **str** | The unique ID of the computer call. | 
**call_id** | **str** | An identifier used when responding to the tool call with output.  | 
**action** | [**ComputerAction**](ComputerAction.md) |  | 
**pending_safety_checks** | [**List[ComputerToolCallSafetyCheck]**](ComputerToolCallSafetyCheck.md) | The pending safety checks for the computer call.  | 
**status** | **str** | The status of the item. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when items are returned via API.  | 

## Example

```python
from openapi_client.models.computer_tool_call import ComputerToolCall

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerToolCall from a JSON string
computer_tool_call_instance = ComputerToolCall.from_json(json)
# print the JSON string representation of the object
print(ComputerToolCall.to_json())

# convert the object into a dict
computer_tool_call_dict = computer_tool_call_instance.to_dict()
# create an instance of ComputerToolCall from a dict
computer_tool_call_from_dict = ComputerToolCall.from_dict(computer_tool_call_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


