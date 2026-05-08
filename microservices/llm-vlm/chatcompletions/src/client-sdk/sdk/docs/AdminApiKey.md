# AdminApiKey

Represents an individual Admin API key in an org.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type, which is always &#x60;organization.admin_api_key&#x60; | 
**id** | **str** | The identifier, which can be referenced in API endpoints | 
**name** | **str** | The name of the API key | 
**redacted_value** | **str** | The redacted value of the API key | 
**value** | **str** | The value of the API key. Only shown on create. | [optional] 
**created_at** | **int** | The Unix timestamp (in seconds) of when the API key was created | 
**last_used_at** | **int** | The Unix timestamp (in seconds) of when the API key was last used | 
**owner** | [**AdminApiKeyOwner**](AdminApiKeyOwner.md) |  | 

## Example

```python
from openapi_client.models.admin_api_key import AdminApiKey

# TODO update the JSON string below
json = "{}"
# create an instance of AdminApiKey from a JSON string
admin_api_key_instance = AdminApiKey.from_json(json)
# print the JSON string representation of the object
print(AdminApiKey.to_json())

# convert the object into a dict
admin_api_key_dict = admin_api_key_instance.to_dict()
# create an instance of AdminApiKey from a dict
admin_api_key_from_dict = AdminApiKey.from_dict(admin_api_key_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


