# ResponseCodeInterpreterCallCodeDeltaEvent

Emitted when a partial code snippet is added by the code interpreter.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.code_interpreter_call.code.delta&#x60;.  | 
**output_index** | **int** | The index of the output item that the code interpreter call is in progress.  | 
**delta** | **str** | The partial code snippet added by the code interpreter.  | 

## Example

```python
from openapi_client.models.response_code_interpreter_call_code_delta_event import ResponseCodeInterpreterCallCodeDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseCodeInterpreterCallCodeDeltaEvent from a JSON string
response_code_interpreter_call_code_delta_event_instance = ResponseCodeInterpreterCallCodeDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseCodeInterpreterCallCodeDeltaEvent.to_json())

# convert the object into a dict
response_code_interpreter_call_code_delta_event_dict = response_code_interpreter_call_code_delta_event_instance.to_dict()
# create an instance of ResponseCodeInterpreterCallCodeDeltaEvent from a dict
response_code_interpreter_call_code_delta_event_from_dict = ResponseCodeInterpreterCallCodeDeltaEvent.from_dict(response_code_interpreter_call_code_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


