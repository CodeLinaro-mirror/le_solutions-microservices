# AuditLogCertificateDeleted

The details for events with this `type`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The certificate ID. | [optional] 
**name** | **str** | The name of the certificate. | [optional] 
**certificate** | **str** | The certificate content in PEM format. | [optional] 

## Example

```python
from openapi_client.models.audit_log_certificate_deleted import AuditLogCertificateDeleted

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCertificateDeleted from a JSON string
audit_log_certificate_deleted_instance = AuditLogCertificateDeleted.from_json(json)
# print the JSON string representation of the object
print(AuditLogCertificateDeleted.to_json())

# convert the object into a dict
audit_log_certificate_deleted_dict = audit_log_certificate_deleted_instance.to_dict()
# create an instance of AuditLogCertificateDeleted from a dict
audit_log_certificate_deleted_from_dict = AuditLogCertificateDeleted.from_dict(audit_log_certificate_deleted_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


