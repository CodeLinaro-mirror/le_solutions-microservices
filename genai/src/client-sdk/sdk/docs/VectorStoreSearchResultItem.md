# VectorStoreSearchResultItem


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file_id** | **str** | The ID of the vector store file. | 
**filename** | **str** | The name of the vector store file. | 
**score** | **float** | The similarity score for the result. | 
**attributes** | [**Dict[str, VectorStoreFileAttributesValue]**](VectorStoreFileAttributesValue.md) | Set of 16 key-value pairs that can be attached to an object. This can be  useful for storing additional information about the object in a structured  format, and querying for objects via API or the dashboard. Keys are strings  with a maximum length of 64 characters. Values are strings with a maximum  length of 512 characters, booleans, or numbers.  | 
**content** | [**List[VectorStoreSearchResultContentObject]**](VectorStoreSearchResultContentObject.md) | Content chunks from the file. | 

## Example

```python
from openapi_client.models.vector_store_search_result_item import VectorStoreSearchResultItem

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreSearchResultItem from a JSON string
vector_store_search_result_item_instance = VectorStoreSearchResultItem.from_json(json)
# print the JSON string representation of the object
print(VectorStoreSearchResultItem.to_json())

# convert the object into a dict
vector_store_search_result_item_dict = vector_store_search_result_item_instance.to_dict()
# create an instance of VectorStoreSearchResultItem from a dict
vector_store_search_result_item_from_dict = VectorStoreSearchResultItem.from_dict(vector_store_search_result_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


