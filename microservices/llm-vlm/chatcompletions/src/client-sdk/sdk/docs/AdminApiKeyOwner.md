# AdminApiKeyOwner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Always &#x60;user&#x60; | [optional] 
**object** | **str** | The object type, which is always organization.user | [optional] 
**id** | **str** | The identifier, which can be referenced in API endpoints | [optional] 
**name** | **str** | The name of the user | [optional] 
**created_at** | **int** | The Unix timestamp (in seconds) of when the user was created | [optional] 
**role** | **str** | Always &#x60;owner&#x60; | [optional] 

## Example

```python
from openapi_client.models.admin_api_key_owner import AdminApiKeyOwner

# TODO update the JSON string below
json = "{}"
# create an instance of AdminApiKeyOwner from a JSON string
admin_api_key_owner_instance = AdminApiKeyOwner.from_json(json)
# print the JSON string representation of the object
print(AdminApiKeyOwner.to_json())

# convert the object into a dict
admin_api_key_owner_dict = admin_api_key_owner_instance.to_dict()
# create an instance of AdminApiKeyOwner from a dict
admin_api_key_owner_from_dict = AdminApiKeyOwner.from_dict(admin_api_key_owner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


