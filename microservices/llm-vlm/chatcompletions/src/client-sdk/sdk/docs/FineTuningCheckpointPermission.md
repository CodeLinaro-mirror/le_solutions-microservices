# FineTuningCheckpointPermission

The `checkpoint.permission` object represents a permission for a fine-tuned model checkpoint. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The permission identifier, which can be referenced in the API endpoints. | 
**created_at** | **int** | The Unix timestamp (in seconds) for when the permission was created. | 
**project_id** | **str** | The project identifier that the permission is for. | 
**object** | **str** | The object type, which is always \&quot;checkpoint.permission\&quot;. | 

## Example

```python
from openapi_client.models.fine_tuning_checkpoint_permission import FineTuningCheckpointPermission

# TODO update the JSON string below
json = "{}"
# create an instance of FineTuningCheckpointPermission from a JSON string
fine_tuning_checkpoint_permission_instance = FineTuningCheckpointPermission.from_json(json)
# print the JSON string representation of the object
print(FineTuningCheckpointPermission.to_json())

# convert the object into a dict
fine_tuning_checkpoint_permission_dict = fine_tuning_checkpoint_permission_instance.to_dict()
# create an instance of FineTuningCheckpointPermission from a dict
fine_tuning_checkpoint_permission_from_dict = FineTuningCheckpointPermission.from_dict(fine_tuning_checkpoint_permission_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


