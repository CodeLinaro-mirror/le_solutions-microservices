# ComputerToolCallOutputResource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the computer tool call output. Always &#x60;computer_call_output&#x60;.  | [default to 'computer_call_output']
**id** | **str** | The unique ID of the computer call tool output.  | 
**call_id** | **str** | The ID of the computer tool call that produced the output.  | 
**acknowledged_safety_checks** | [**List[ComputerToolCallSafetyCheck]**](ComputerToolCallSafetyCheck.md) | The safety checks reported by the API that have been acknowledged by the  developer.  | [optional] 
**output** | [**ComputerScreenshotImage**](ComputerScreenshotImage.md) |  | 
**status** | **str** | The status of the message input. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when input items are returned via API.  | [optional] 

## Example

```python
from openapi_client.models.computer_tool_call_output_resource import ComputerToolCallOutputResource

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerToolCallOutputResource from a JSON string
computer_tool_call_output_resource_instance = ComputerToolCallOutputResource.from_json(json)
# print the JSON string representation of the object
print(ComputerToolCallOutputResource.to_json())

# convert the object into a dict
computer_tool_call_output_resource_dict = computer_tool_call_output_resource_instance.to_dict()
# create an instance of ComputerToolCallOutputResource from a dict
computer_tool_call_output_resource_from_dict = ComputerToolCallOutputResource.from_dict(computer_tool_call_output_resource_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


