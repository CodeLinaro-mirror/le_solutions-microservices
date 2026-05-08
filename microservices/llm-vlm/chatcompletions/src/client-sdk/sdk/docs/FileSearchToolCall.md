# FileSearchToolCall

The results of a file search tool call. See the  [file search guide](/docs/guides/tools-file-search) for more information. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The unique ID of the file search tool call.  | 
**type** | **str** | The type of the file search tool call. Always &#x60;file_search_call&#x60;.  | 
**status** | **str** | The status of the file search tool call. One of &#x60;in_progress&#x60;,  &#x60;searching&#x60;, &#x60;incomplete&#x60; or &#x60;failed&#x60;,  | 
**queries** | **List[str]** | The queries used to search for files.  | 
**results** | [**List[FileSearchToolCallResultsInner]**](FileSearchToolCallResultsInner.md) | The results of the file search tool call.  | [optional] 

## Example

```python
from openapi_client.models.file_search_tool_call import FileSearchToolCall

# TODO update the JSON string below
json = "{}"
# create an instance of FileSearchToolCall from a JSON string
file_search_tool_call_instance = FileSearchToolCall.from_json(json)
# print the JSON string representation of the object
print(FileSearchToolCall.to_json())

# convert the object into a dict
file_search_tool_call_dict = file_search_tool_call_instance.to_dict()
# create an instance of FileSearchToolCall from a dict
file_search_tool_call_from_dict = FileSearchToolCall.from_dict(file_search_tool_call_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


