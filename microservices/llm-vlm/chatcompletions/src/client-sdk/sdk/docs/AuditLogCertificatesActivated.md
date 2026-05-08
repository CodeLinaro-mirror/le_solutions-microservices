# AuditLogCertificatesActivated

The details for events with this `type`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**certificates** | [**List[AuditLogCertificatesActivatedCertificatesInner]**](AuditLogCertificatesActivatedCertificatesInner.md) |  | [optional] 

## Example

```python
from openapi_client.models.audit_log_certificates_activated import AuditLogCertificatesActivated

# TODO update the JSON string below
json = "{}"
# create an instance of AuditLogCertificatesActivated from a JSON string
audit_log_certificates_activated_instance = AuditLogCertificatesActivated.from_json(json)
# print the JSON string representation of the object
print(AuditLogCertificatesActivated.to_json())

# convert the object into a dict
audit_log_certificates_activated_dict = audit_log_certificates_activated_instance.to_dict()
# create an instance of AuditLogCertificatesActivated from a dict
audit_log_certificates_activated_from_dict = AuditLogCertificatesActivated.from_dict(audit_log_certificates_activated_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


