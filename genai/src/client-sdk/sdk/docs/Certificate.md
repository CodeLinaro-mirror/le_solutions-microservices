# Certificate

Represents an individual `certificate` uploaded to the organization.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type.  - If creating, updating, or getting a specific certificate, the object type is &#x60;certificate&#x60;. - If listing, activating, or deactivating certificates for the organization, the object type is &#x60;organization.certificate&#x60;. - If listing, activating, or deactivating certificates for a project, the object type is &#x60;organization.project.certificate&#x60;.  | 
**id** | **str** | The identifier, which can be referenced in API endpoints | 
**name** | **str** | The name of the certificate. | 
**created_at** | **int** | The Unix timestamp (in seconds) of when the certificate was uploaded. | 
**certificate_details** | [**CertificateCertificateDetails**](CertificateCertificateDetails.md) |  | 
**active** | **bool** | Whether the certificate is currently active at the specified scope. Not returned when getting details for a specific certificate. | [optional] 

## Example

```python
from openapi_client.models.certificate import Certificate

# TODO update the JSON string below
json = "{}"
# create an instance of Certificate from a JSON string
certificate_instance = Certificate.from_json(json)
# print the JSON string representation of the object
print(Certificate.to_json())

# convert the object into a dict
certificate_dict = certificate_instance.to_dict()
# create an instance of Certificate from a dict
certificate_from_dict = Certificate.from_dict(certificate_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


