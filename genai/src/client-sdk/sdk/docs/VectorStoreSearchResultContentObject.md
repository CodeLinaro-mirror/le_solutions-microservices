# VectorStoreSearchResultContentObject


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of content. | 
**text** | **str** | The text content returned from search. | 

## Example

```python
from openapi_client.models.vector_store_search_result_content_object import VectorStoreSearchResultContentObject

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreSearchResultContentObject from a JSON string
vector_store_search_result_content_object_instance = VectorStoreSearchResultContentObject.from_json(json)
# print the JSON string representation of the object
print(VectorStoreSearchResultContentObject.to_json())

# convert the object into a dict
vector_store_search_result_content_object_dict = vector_store_search_result_content_object_instance.to_dict()
# create an instance of VectorStoreSearchResultContentObject from a dict
vector_store_search_result_content_object_from_dict = VectorStoreSearchResultContentObject.from_dict(vector_store_search_result_content_object_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


