# UpdateVectorStoreFileAttributesRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**attributes** | [**Dict[str, VectorStoreFileAttributesValue]**](VectorStoreFileAttributesValue.md) | Set of 16 key-value pairs that can be attached to an object. This can be  useful for storing additional information about the object in a structured  format, and querying for objects via API or the dashboard. Keys are strings  with a maximum length of 64 characters. Values are strings with a maximum  length of 512 characters, booleans, or numbers.  | 

## Example

```python
from openapi_client.models.update_vector_store_file_attributes_request import UpdateVectorStoreFileAttributesRequest

# TODO update the JSON string below
json = "{}"
# create an instance of UpdateVectorStoreFileAttributesRequest from a JSON string
update_vector_store_file_attributes_request_instance = UpdateVectorStoreFileAttributesRequest.from_json(json)
# print the JSON string representation of the object
print(UpdateVectorStoreFileAttributesRequest.to_json())

# convert the object into a dict
update_vector_store_file_attributes_request_dict = update_vector_store_file_attributes_request_instance.to_dict()
# create an instance of UpdateVectorStoreFileAttributesRequest from a dict
update_vector_store_file_attributes_request_from_dict = UpdateVectorStoreFileAttributesRequest.from_dict(update_vector_store_file_attributes_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


