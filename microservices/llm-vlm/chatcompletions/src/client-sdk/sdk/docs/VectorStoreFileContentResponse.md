# VectorStoreFileContentResponse

Represents the parsed content of a vector store file.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type, which is always &#x60;vector_store.file_content.page&#x60; | 
**data** | [**List[VectorStoreFileContentResponseDataInner]**](VectorStoreFileContentResponseDataInner.md) | Parsed content of the file. | 
**has_more** | **bool** | Indicates if there are more content pages to fetch. | 
**next_page** | **str** | The token for the next page, if any. | 

## Example

```python
from openapi_client.models.vector_store_file_content_response import VectorStoreFileContentResponse

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreFileContentResponse from a JSON string
vector_store_file_content_response_instance = VectorStoreFileContentResponse.from_json(json)
# print the JSON string representation of the object
print(VectorStoreFileContentResponse.to_json())

# convert the object into a dict
vector_store_file_content_response_dict = vector_store_file_content_response_instance.to_dict()
# create an instance of VectorStoreFileContentResponse from a dict
vector_store_file_content_response_from_dict = VectorStoreFileContentResponse.from_dict(vector_store_file_content_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


