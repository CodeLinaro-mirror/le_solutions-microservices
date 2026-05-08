# CodeInterpreterTextOutput

The output of a code interpreter tool call that is text. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the code interpreter text output. Always &#x60;logs&#x60;.  | 
**logs** | **str** | The logs of the code interpreter tool call.  | 

## Example

```python
from openapi_client.models.code_interpreter_text_output import CodeInterpreterTextOutput

# TODO update the JSON string below
json = "{}"
# create an instance of CodeInterpreterTextOutput from a JSON string
code_interpreter_text_output_instance = CodeInterpreterTextOutput.from_json(json)
# print the JSON string representation of the object
print(CodeInterpreterTextOutput.to_json())

# convert the object into a dict
code_interpreter_text_output_dict = code_interpreter_text_output_instance.to_dict()
# create an instance of CodeInterpreterTextOutput from a dict
code_interpreter_text_output_from_dict = CodeInterpreterTextOutput.from_dict(code_interpreter_text_output_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


