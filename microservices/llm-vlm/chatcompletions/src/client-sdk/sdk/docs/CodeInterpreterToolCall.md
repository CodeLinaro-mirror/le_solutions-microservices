# CodeInterpreterToolCall

A tool call to run code. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The unique ID of the code interpreter tool call.  | 
**type** | **str** | The type of the code interpreter tool call. Always &#x60;code_interpreter_call&#x60;.  | 
**code** | **str** | The code to run.  | 
**status** | **str** | The status of the code interpreter tool call.  | 
**results** | [**List[CodeInterpreterToolOutput]**](CodeInterpreterToolOutput.md) | The results of the code interpreter tool call.  | 

## Example

```python
from openapi_client.models.code_interpreter_tool_call import CodeInterpreterToolCall

# TODO update the JSON string below
json = "{}"
# create an instance of CodeInterpreterToolCall from a JSON string
code_interpreter_tool_call_instance = CodeInterpreterToolCall.from_json(json)
# print the JSON string representation of the object
print(CodeInterpreterToolCall.to_json())

# convert the object into a dict
code_interpreter_tool_call_dict = code_interpreter_tool_call_instance.to_dict()
# create an instance of CodeInterpreterToolCall from a dict
code_interpreter_tool_call_from_dict = CodeInterpreterToolCall.from_dict(code_interpreter_tool_call_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


