# FileSearchTool

A tool that searches for relevant content from uploaded files.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the file search tool. Always &#x60;file_search&#x60;. | [default to 'file_search']
**vector_store_ids** | **List[str]** | The IDs of the vector stores to search. | 
**max_num_results** | **int** | The maximum number of results to return. This number should be between 1 and 50 inclusive. | [optional] 
**ranking_options** | [**RankingOptions**](RankingOptions.md) |  | [optional] 
**filters** | [**Filters**](Filters.md) |  | [optional] 

## Example

```python
from openapi_client.models.file_search_tool import FileSearchTool

# TODO update the JSON string below
json = "{}"
# create an instance of FileSearchTool from a JSON string
file_search_tool_instance = FileSearchTool.from_json(json)
# print the JSON string representation of the object
print(FileSearchTool.to_json())

# convert the object into a dict
file_search_tool_dict = file_search_tool_instance.to_dict()
# create an instance of FileSearchTool from a dict
file_search_tool_from_dict = FileSearchTool.from_dict(file_search_tool_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


