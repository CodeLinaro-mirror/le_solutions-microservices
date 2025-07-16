# ComputerCallSafetyCheckParam

A pending safety check for the computer call.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the pending safety check. | 
**code** | **str** | The type of the pending safety check. | [optional] 
**message** | **str** | Details about the pending safety check. | [optional] 

## Example

```python
from openapi_client.models.computer_call_safety_check_param import ComputerCallSafetyCheckParam

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerCallSafetyCheckParam from a JSON string
computer_call_safety_check_param_instance = ComputerCallSafetyCheckParam.from_json(json)
# print the JSON string representation of the object
print(ComputerCallSafetyCheckParam.to_json())

# convert the object into a dict
computer_call_safety_check_param_dict = computer_call_safety_check_param_instance.to_dict()
# create an instance of ComputerCallSafetyCheckParam from a dict
computer_call_safety_check_param_from_dict = ComputerCallSafetyCheckParam.from_dict(computer_call_safety_check_param_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


