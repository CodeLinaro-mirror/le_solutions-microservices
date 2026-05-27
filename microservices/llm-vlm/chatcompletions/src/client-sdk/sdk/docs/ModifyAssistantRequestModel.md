# ModifyAssistantRequestModel

ID of the model to use. You can use the [List models](/docs/api-reference/models/list) API to see all of your available models, or see our [Model overview](/docs/models) for descriptions of them. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.modify_assistant_request_model import ModifyAssistantRequestModel

# TODO update the JSON string below
json = "{}"
# create an instance of ModifyAssistantRequestModel from a JSON string
modify_assistant_request_model_instance = ModifyAssistantRequestModel.from_json(json)
# print the JSON string representation of the object
print(ModifyAssistantRequestModel.to_json())

# convert the object into a dict
modify_assistant_request_model_dict = modify_assistant_request_model_instance.to_dict()
# create an instance of ModifyAssistantRequestModel from a dict
modify_assistant_request_model_from_dict = ModifyAssistantRequestModel.from_dict(modify_assistant_request_model_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


