# AuditLogCheckpointPermissionDeleted

The details for events with this `type`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The ID of the checkpoint permission. | [optional] 

## Example

```python
from openapi_client.models.audit_log_checkpoint_permission_deleted import AuditLogCheckpointPermissionDeleted

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCheckpointPermissionDeleted from a JSON string
audit_log_checkpoint_permission_deleted_instance = AuditLogCheckpointPermissionDeleted.from_json(json)
# print the JSON string representation of the object
print(AuditLogCheckpointPermissionDeleted.to_json())

# convert the object into a dict
audit_log_checkpoint_permission_deleted_dict = audit_log_checkpoint_permission_deleted_instance.to_dict()
# create an instance of AuditLogCheckpointPermissionDeleted from a dict
audit_log_checkpoint_permission_deleted_from_dict = AuditLogCheckpointPermissionDeleted.from_dict(audit_log_checkpoint_permission_deleted_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


