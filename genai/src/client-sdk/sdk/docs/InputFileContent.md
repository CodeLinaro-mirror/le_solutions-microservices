# InputFileContent

A file input to the model.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_file&#x60;. | [default to 'input_file']
**file_id** | **str** | The ID of the file to be sent to the model. | [optional] 
**filename** | **str** | The name of the file to be sent to the model. | [optional] 
**file_data** | **str** | The content of the file to be sent to the model.  | [optional] 

## Example

```python
from openapi_client.models.input_file_content import InputFileContent

# TODO update the JSON string below
json = "{}"
# create an instance of InputFileContent from a JSON string
input_file_content_instance = InputFileContent.from_json(json)
# print the JSON string representation of the object
print(InputFileContent.to_json())

# convert the object into a dict
input_file_content_dict = input_file_content_instance.to_dict()
# create an instance of InputFileContent from a dict
input_file_content_from_dict = InputFileContent.from_dict(input_file_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


