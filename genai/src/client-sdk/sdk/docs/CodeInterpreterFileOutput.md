# CodeInterpreterFileOutput

The output of a code interpreter tool call that is a file. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the code interpreter file output. Always &#x60;files&#x60;.  | 
**files** | [**List[CodeInterpreterFileOutputFilesInner]**](CodeInterpreterFileOutputFilesInner.md) |  | 

## Example

```python
from openapi_client.models.code_interpreter_file_output import CodeInterpreterFileOutput

# TODO update the JSON string below
json = "{}"
# create an instance of CodeInterpreterFileOutput from a JSON string
code_interpreter_file_output_instance = CodeInterpreterFileOutput.from_json(json)
# print the JSON string representation of the object
print(CodeInterpreterFileOutput.to_json())

# convert the object into a dict
code_interpreter_file_output_dict = code_interpreter_file_output_instance.to_dict()
# create an instance of CodeInterpreterFileOutput from a dict
code_interpreter_file_output_from_dict = CodeInterpreterFileOutput.from_dict(code_interpreter_file_output_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


