# AuditLogCheckpointPermissionCreatedData

The payload used to create the checkpoint permission.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**project_id** | **str** | The ID of the project that the checkpoint permission was created for. | [optional] 
**fine_tuned_model_checkpoint** | **str** | The ID of the fine-tuned model checkpoint. | [optional] 

## Example

```python
from openapi_client.models.audit_log_checkpoint_permission_created_data import AuditLogCheckpointPermissionCreatedData

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCheckpointPermissionCreatedData from a JSON string
audit_log_checkpoint_permission_created_data_instance = AuditLogCheckpointPermissionCreatedData.from_json(json)
# print the JSON string representation of the object
print(AuditLogCheckpointPermissionCreatedData.to_json())

# convert the object into a dict
audit_log_checkpoint_permission_created_data_dict = audit_log_checkpoint_permission_created_data_instance.to_dict()
# create an instance of AuditLogCheckpointPermissionCreatedData from a dict
audit_log_checkpoint_permission_created_data_from_dict = AuditLogCheckpointPermissionCreatedData.from_dict(audit_log_checkpoint_permission_created_data_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


