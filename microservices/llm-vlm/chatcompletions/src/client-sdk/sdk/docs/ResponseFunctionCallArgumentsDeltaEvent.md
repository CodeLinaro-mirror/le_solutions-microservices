# ResponseFunctionCallArgumentsDeltaEvent

Emitted when there is a partial function-call arguments delta.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.function_call_arguments.delta&#x60;.  | 
**item_id** | **str** | The ID of the output item that the function-call arguments delta is added to.  | 
**output_index** | **int** | The index of the output item that the function-call arguments delta is added to.  | 
**delta** | **str** | The function-call arguments delta that is added.  | 

## Example

```python
from openapi_client.models.response_function_call_arguments_delta_event import ResponseFunctionCallArgumentsDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseFunctionCallArgumentsDeltaEvent from a JSON string
response_function_call_arguments_delta_event_instance = ResponseFunctionCallArgumentsDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseFunctionCallArgumentsDeltaEvent.to_json())

# convert the object into a dict
response_function_call_arguments_delta_event_dict = response_function_call_arguments_delta_event_instance.to_dict()
# create an instance of ResponseFunctionCallArgumentsDeltaEvent from a dict
response_function_call_arguments_delta_event_from_dict = ResponseFunctionCallArgumentsDeltaEvent.from_dict(response_function_call_arguments_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


