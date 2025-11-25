# UpdateVectorStoreRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**name** | **str** | The name of the vector store. | [optional] 
**expires_after** | [**UpdateVectorStoreRequestExpiresAfter**](UpdateVectorStoreRequestExpiresAfter.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.update_vector_store_request import UpdateVectorStoreRequest

# TODO update the JSON string below
json = "{}"
# create an instance of UpdateVectorStoreRequest from a JSON string
update_vector_store_request_instance = UpdateVectorStoreRequest.from_json(json)
# print the JSON string representation of the object
print(UpdateVectorStoreRequest.to_json())

# convert the object into a dict
update_vector_store_request_dict = update_vector_store_request_instance.to_dict()
# create an instance of UpdateVectorStoreRequest from a dict
update_vector_store_request_from_dict = UpdateVectorStoreRequest.from_dict(update_vector_store_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


