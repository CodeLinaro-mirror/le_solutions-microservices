# ModifyThreadRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**tool_resources** | [**ModifyThreadRequestToolResources**](ModifyThreadRequestToolResources.md) |  | [optional] 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.modify_thread_request import ModifyThreadRequest

# TODO update the JSON string below
json = "{}"
# create an instance of ModifyThreadRequest from a JSON string
modify_thread_request_instance = ModifyThreadRequest.from_json(json)
# print the JSON string representation of the object
print(ModifyThreadRequest.to_json())

# convert the object into a dict
modify_thread_request_dict = modify_thread_request_instance.to_dict()
# create an instance of ModifyThreadRequest from a dict
modify_thread_request_from_dict = ModifyThreadRequest.from_dict(modify_thread_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


