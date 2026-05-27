# UpdateVectorStoreRequestExpiresAfter


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**anchor** | **str** | Anchor timestamp after which the expiration policy applies. Supported anchors: &#x60;last_active_at&#x60;. | 
**days** | **int** | The number of days after the anchor time that the vector store will expire. | 

## Example

```python
from openapi_client.models.update_vector_store_request_expires_after import UpdateVectorStoreRequestExpiresAfter

# TODO update the JSON string below
json = "{}"
# create an instance of UpdateVectorStoreRequestExpiresAfter from a JSON string
update_vector_store_request_expires_after_instance = UpdateVectorStoreRequestExpiresAfter.from_json(json)
# print the JSON string representation of the object
print(UpdateVectorStoreRequestExpiresAfter.to_json())

# convert the object into a dict
update_vector_store_request_expires_after_dict = update_vector_store_request_expires_after_instance.to_dict()
# create an instance of UpdateVectorStoreRequestExpiresAfter from a dict
update_vector_store_request_expires_after_from_dict = UpdateVectorStoreRequestExpiresAfter.from_dict(update_vector_store_request_expires_after_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


