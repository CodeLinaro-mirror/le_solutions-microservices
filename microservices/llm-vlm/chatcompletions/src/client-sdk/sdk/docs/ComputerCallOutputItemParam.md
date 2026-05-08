# ComputerCallOutputItemParam

The output of a computer tool call.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the computer tool call output. | [optional] 
**call_id** | **str** | The ID of the computer tool call that produced the output. | 
**type** | **str** | The type of the computer tool call output. Always &#x60;computer_call_output&#x60;. | [default to 'computer_call_output']
**output** | [**ComputerScreenshotImage**](ComputerScreenshotImage.md) |  | 
**acknowledged_safety_checks** | [**List[ComputerCallSafetyCheckParam]**](ComputerCallSafetyCheckParam.md) | The safety checks reported by the API that have been acknowledged by the developer. | [optional] 
**status** | **str** | The status of the message input. One of &#x60;in_progress&#x60;, &#x60;completed&#x60;, or &#x60;incomplete&#x60;. Populated when input items are returned via API. | [optional] 

## Example

```python
from openapi_client.models.computer_call_output_item_param import ComputerCallOutputItemParam

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerCallOutputItemParam from a JSON string
computer_call_output_item_param_instance = ComputerCallOutputItemParam.from_json(json)
# print the JSON string representation of the object
print(ComputerCallOutputItemParam.to_json())

# convert the object into a dict
computer_call_output_item_param_dict = computer_call_output_item_param_instance.to_dict()
# create an instance of ComputerCallOutputItemParam from a dict
computer_call_output_item_param_from_dict = ComputerCallOutputItemParam.from_dict(computer_call_output_item_param_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


