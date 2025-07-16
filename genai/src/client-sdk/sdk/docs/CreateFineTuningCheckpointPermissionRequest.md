# CreateFineTuningCheckpointPermissionRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**project_ids** | **List[str]** | The project identifiers to grant access to. | 

## Example

```python
from openapi_client.models.create_fine_tuning_checkpoint_permission_request import CreateFineTuningCheckpointPermissionRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateFineTuningCheckpointPermissionRequest from a JSON string
create_fine_tuning_checkpoint_permission_request_instance = CreateFineTuningCheckpointPermissionRequest.from_json(json)
# print the JSON string representation of the object
print(CreateFineTuningCheckpointPermissionRequest.to_json())

# convert the object into a dict
create_fine_tuning_checkpoint_permission_request_dict = create_fine_tuning_checkpoint_permission_request_instance.to_dict()
# create an instance of CreateFineTuningCheckpointPermissionRequest from a dict
create_fine_tuning_checkpoint_permission_request_from_dict = CreateFineTuningCheckpointPermissionRequest.from_dict(create_fine_tuning_checkpoint_permission_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


