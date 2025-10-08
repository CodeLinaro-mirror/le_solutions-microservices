# CreateAssistantRequestToolResourcesFileSearchVectorStoresInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file_ids** | **List[str]** | A list of [file](/docs/api-reference/files) IDs to add to the vector store. There can be a maximum of 10000 files in a vector store.  | [optional] 
**chunking_strategy** | [**CreateAssistantRequestToolResourcesFileSearchVectorStoresInnerChunkingStrategy**](CreateAssistantRequestToolResourcesFileSearchVectorStoresInnerChunkingStrategy.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.create_assistant_request_tool_resources_file_search_vector_stores_inner import CreateAssistantRequestToolResourcesFileSearchVectorStoresInner

# TODO update the JSON string below
json = "{}"
# create an instance of CreateAssistantRequestToolResourcesFileSearchVectorStoresInner from a JSON string
create_assistant_request_tool_resources_file_search_vector_stores_inner_instance = CreateAssistantRequestToolResourcesFileSearchVectorStoresInner.from_json(json)
# print the JSON string representation of the object
print(CreateAssistantRequestToolResourcesFileSearchVectorStoresInner.to_json())

# convert the object into a dict
create_assistant_request_tool_resources_file_search_vector_stores_inner_dict = create_assistant_request_tool_resources_file_search_vector_stores_inner_instance.to_dict()
# create an instance of CreateAssistantRequestToolResourcesFileSearchVectorStoresInner from a dict
create_assistant_request_tool_resources_file_search_vector_stores_inner_from_dict = CreateAssistantRequestToolResourcesFileSearchVectorStoresInner.from_dict(create_assistant_request_tool_resources_file_search_vector_stores_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


