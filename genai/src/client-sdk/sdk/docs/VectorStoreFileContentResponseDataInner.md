# VectorStoreFileContentResponseDataInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The content type (currently only &#x60;\&quot;text\&quot;&#x60;) | [optional] 
**text** | **str** | The text content | [optional] 

## Example

```python
from openapi_client.models.vector_store_file_content_response_data_inner import VectorStoreFileContentResponseDataInner

# TODO update the JSON string below
json = "{}"
# create an instance of VectorStoreFileContentResponseDataInner from a JSON string
vector_store_file_content_response_data_inner_instance = VectorStoreFileContentResponseDataInner.from_json(json)
# print the JSON string representation of the object
print(VectorStoreFileContentResponseDataInner.to_json())

# convert the object into a dict
vector_store_file_content_response_data_inner_dict = vector_store_file_content_response_data_inner_instance.to_dict()
# create an instance of VectorStoreFileContentResponseDataInner from a dict
vector_store_file_content_response_data_inner_from_dict = VectorStoreFileContentResponseDataInner.from_dict(vector_store_file_content_response_data_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


