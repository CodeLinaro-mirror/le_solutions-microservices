# AuditLogCheckpointPermissionCreated

The project and fine-tuned model checkpoint that the checkpoint permission was created for.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the checkpoint permission. | [optional] 
**data** | [**AuditLogCheckpointPermissionCreatedData**](AuditLogCheckpointPermissionCreatedData.md) |  | [optional] 

## Example

```python
from openapi_client.models.audit_log_checkpoint_permission_created import AuditLogCheckpointPermissionCreated

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCheckpointPermissionCreated from a JSON string
audit_log_checkpoint_permission_created_instance = AuditLogCheckpointPermissionCreated.from_json(json)
# print the JSON string representation of the object
print(AuditLogCheckpointPermissionCreated.to_json())

# convert the object into a dict
audit_log_checkpoint_permission_created_dict = audit_log_checkpoint_permission_created_instance.to_dict()
# create an instance of AuditLogCheckpointPermissionCreated from a dict
audit_log_checkpoint_permission_created_from_dict = AuditLogCheckpointPermissionCreated.from_dict(audit_log_checkpoint_permission_created_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


