# ResponseFunctionCallArgumentsDoneEvent

Emitted when function-call arguments are finalized.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** |  | 
**item_id** | **str** | The ID of the item. | 
**output_index** | **int** | The index of the output item. | 
**arguments** | **str** | The function-call arguments. | 

## Example

```python
from openapi_client.models.response_function_call_arguments_done_event import ResponseFunctionCallArgumentsDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseFunctionCallArgumentsDoneEvent from a JSON string
response_function_call_arguments_done_event_instance = ResponseFunctionCallArgumentsDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseFunctionCallArgumentsDoneEvent.to_json())

# convert the object into a dict
response_function_call_arguments_done_event_dict = response_function_call_arguments_done_event_instance.to_dict()
# create an instance of ResponseFunctionCallArgumentsDoneEvent from a dict
response_function_call_arguments_done_event_from_dict = ResponseFunctionCallArgumentsDoneEvent.from_dict(response_function_call_arguments_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


