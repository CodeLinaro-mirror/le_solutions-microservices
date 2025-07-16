# AuditLogCertificateCreated

The details for events with this `type`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The certificate ID. | [optional] 
**name** | **str** | The name of the certificate. | [optional] 

## Example

```python
from openapi_client.models.audit_log_certificate_created import AuditLogCertificateCreated

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCertificateCreated from a JSON string
audit_log_certificate_created_instance = AuditLogCertificateCreated.from_json(json)
# print the JSON string representation of the object
print(AuditLogCertificateCreated.to_json())

# convert the object into a dict
audit_log_certificate_created_dict = audit_log_certificate_created_instance.to_dict()
# create an instance of AuditLogCertificateCreated from a dict
audit_log_certificate_created_from_dict = AuditLogCertificateCreated.from_dict(audit_log_certificate_created_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


