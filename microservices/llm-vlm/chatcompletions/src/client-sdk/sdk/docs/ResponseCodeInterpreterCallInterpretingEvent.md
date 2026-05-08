# ResponseCodeInterpreterCallInterpretingEvent

Emitted when the code interpreter is actively interpreting the code snippet.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.code_interpreter_call.interpreting&#x60;.  | 
**output_index** | **int** | The index of the output item that the code interpreter call is in progress.  | 
**code_interpreter_call** | [**CodeInterpreterToolCall**](CodeInterpreterToolCall.md) |  | 

## Example

```python
from openapi_client.models.response_code_interpreter_call_interpreting_event import ResponseCodeInterpreterCallInterpretingEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseCodeInterpreterCallInterpretingEvent from a JSON string
response_code_interpreter_call_interpreting_event_instance = ResponseCodeInterpreterCallInterpretingEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseCodeInterpreterCallInterpretingEvent.to_json())

# convert the object into a dict
response_code_interpreter_call_interpreting_event_dict = response_code_interpreter_call_interpreting_event_instance.to_dict()
# create an instance of ResponseCodeInterpreterCallInterpretingEvent from a dict
response_code_interpreter_call_interpreting_event_from_dict = ResponseCodeInterpreterCallInterpretingEvent.from_dict(response_code_interpreter_call_interpreting_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


