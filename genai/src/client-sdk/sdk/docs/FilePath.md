# FilePath

A path to a file. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the file path. Always &#x60;file_path&#x60;.  | 
**file_id** | **str** | The ID of the file.  | 
**index** | **int** | The index of the file in the list of files.  | 

## Example

```python
from openapi_client.models.file_path import FilePath

# TODO update the JSON string below
json = "{}"
# create an instance of FilePath from a JSON string
file_path_instance = FilePath.from_json(json)
# print the JSON string representation of the object
print(FilePath.to_json())

# convert the object into a dict
file_path_dict = file_path_instance.to_dict()
# create an instance of FilePath from a dict
file_path_from_dict = FilePath.from_dict(file_path_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


