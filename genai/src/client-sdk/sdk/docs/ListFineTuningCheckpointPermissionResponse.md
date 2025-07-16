# ListFineTuningCheckpointPermissionResponse


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**data** | [**List[FineTuningCheckpointPermission]**](FineTuningCheckpointPermission.md) |  | 
**object** | **str** |  | 
**first_id** | **str** |  | [optional] 
**last_id** | **str** |  | [optional] 
**has_more** | **bool** |  | 

## Example

```python
from openapi_client.models.list_fine_tuning_checkpoint_permission_response import ListFineTuningCheckpointPermissionResponse

# TODO update the JSON string below
json = "{}"
# create an instance of ListFineTuningCheckpointPermissionResponse from a JSON string
list_fine_tuning_checkpoint_permission_response_instance = ListFineTuningCheckpointPermissionResponse.from_json(json)
# print the JSON string representation of the object
print(ListFineTuningCheckpointPermissionResponse.to_json())

# convert the object into a dict
list_fine_tuning_checkpoint_permission_response_dict = list_fine_tuning_checkpoint_permission_response_instance.to_dict()
# create an instance of ListFineTuningCheckpointPermissionResponse from a dict
list_fine_tuning_checkpoint_permission_response_from_dict = ListFineTuningCheckpointPermissionResponse.from_dict(list_fine_tuning_checkpoint_permission_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


