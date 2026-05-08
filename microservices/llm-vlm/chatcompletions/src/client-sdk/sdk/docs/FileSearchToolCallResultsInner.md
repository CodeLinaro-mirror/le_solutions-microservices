# FileSearchToolCallResultsInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file_id** | **str** | The unique ID of the file.  | [optional] 
**text** | **str** | The text that was retrieved from the file.  | [optional] 
**filename** | **str** | The name of the file.  | [optional] 
**attributes** | [**Dict[str, VectorStoreFileAttributesValue]**](VectorStoreFileAttributesValue.md) | Set of 16 key-value pairs that can be attached to an object. This can be  useful for storing additional information about the object in a structured  format, and querying for objects via API or the dashboard. Keys are strings  with a maximum length of 64 characters. Values are strings with a maximum  length of 512 characters, booleans, or numbers.  | [optional] 
**score** | **float** | The relevance score of the file - a value between 0 and 1.  | [optional] 

## Example

```python
from openapi_client.models.file_search_tool_call_results_inner import FileSearchToolCallResultsInner

# TODO update the JSON string below
json = "{}"
# create an instance of FileSearchToolCallResultsInner from a JSON string
file_search_tool_call_results_inner_instance = FileSearchToolCallResultsInner.from_json(json)
# print the JSON string representation of the object
print(FileSearchToolCallResultsInner.to_json())

# convert the object into a dict
file_search_tool_call_results_inner_dict = file_search_tool_call_results_inner_instance.to_dict()
# create an instance of FileSearchToolCallResultsInner from a dict
file_search_tool_call_results_inner_from_dict = FileSearchToolCallResultsInner.from_dict(file_search_tool_call_results_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


