# CodeInterpreterToolOutput


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the code interpreter text output. Always &#x60;logs&#x60;.  | 
**logs** | **str** | The logs of the code interpreter tool call.  | 
**files** | [**List[CodeInterpreterFileOutputFilesInner]**](CodeInterpreterFileOutputFilesInner.md) |  | 

## Example

```python
from openapi_client.models.code_interpreter_tool_output import CodeInterpreterToolOutput

# TODO update the JSON string below
json = "{}"
# create an instance of CodeInterpreterToolOutput from a JSON string
code_interpreter_tool_output_instance = CodeInterpreterToolOutput.from_json(json)
# print the JSON string representation of the object
print(CodeInterpreterToolOutput.to_json())

# convert the object into a dict
code_interpreter_tool_output_dict = code_interpreter_tool_output_instance.to_dict()
# create an instance of CodeInterpreterToolOutput from a dict
code_interpreter_tool_output_from_dict = CodeInterpreterToolOutput.from_dict(code_interpreter_tool_output_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


