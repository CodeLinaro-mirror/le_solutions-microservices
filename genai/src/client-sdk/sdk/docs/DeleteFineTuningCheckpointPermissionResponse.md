# DeleteFineTuningCheckpointPermissionResponse


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the fine-tuned model checkpoint permission that was deleted. | 
**object** | **str** | The object type, which is always \&quot;checkpoint.permission\&quot;. | 
**deleted** | **bool** | Whether the fine-tuned model checkpoint permission was successfully deleted. | 

## Example

```python
from openapi_client.models.delete_fine_tuning_checkpoint_permission_response import DeleteFineTuningCheckpointPermissionResponse

# TODO update the JSON string below
json = "{}"
# create an instance of DeleteFineTuningCheckpointPermissionResponse from a JSON string
delete_fine_tuning_checkpoint_permission_response_instance = DeleteFineTuningCheckpointPermissionResponse.from_json(json)
# print the JSON string representation of the object
print(DeleteFineTuningCheckpointPermissionResponse.to_json())

# convert the object into a dict
delete_fine_tuning_checkpoint_permission_response_dict = delete_fine_tuning_checkpoint_permission_response_instance.to_dict()
# create an instance of DeleteFineTuningCheckpointPermissionResponse from a dict
delete_fine_tuning_checkpoint_permission_response_from_dict = DeleteFineTuningCheckpointPermissionResponse.from_dict(delete_fine_tuning_checkpoint_permission_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


