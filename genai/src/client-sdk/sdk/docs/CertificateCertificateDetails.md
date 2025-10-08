# CertificateCertificateDetails


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**valid_at** | **int** | The Unix timestamp (in seconds) of when the certificate becomes valid. | [optional] 
**expires_at** | **int** | The Unix timestamp (in seconds) of when the certificate expires. | [optional] 
**content** | **str** | The content of the certificate in PEM format. | [optional] 

## Example

```python
from openapi_client.models.certificate_certificate_details import CertificateCertificateDetails

# TODO update the JSON string below
json = "{}"
# create an instance of CertificateCertificateDetails from a JSON string
certificate_certificate_details_instance = CertificateCertificateDetails.from_json(json)
# print the JSON string representation of the object
print(CertificateCertificateDetails.to_json())

# convert the object into a dict
certificate_certificate_details_dict = certificate_certificate_details_instance.to_dict()
# create an instance of CertificateCertificateDetails from a dict
certificate_certificate_details_from_dict = CertificateCertificateDetails.from_dict(certificate_certificate_details_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


