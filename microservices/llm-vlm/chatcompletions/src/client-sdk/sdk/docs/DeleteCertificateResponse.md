# DeleteCertificateResponse


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type, must be &#x60;certificate.deleted&#x60;. | 
**id** | **str** | The ID of the certificate that was deleted. | 

## Example

```python
from openapi_client.models.delete_certificate_response import DeleteCertificateResponse

# TODO update the JSON string below
json = "{}"
# create an instance of DeleteCertificateResponse from a JSON string
delete_certificate_response_instance = DeleteCertificateResponse.from_json(json)
# print the JSON string representation of the object
print(DeleteCertificateResponse.to_json())

# convert the object into a dict
delete_certificate_response_dict = delete_certificate_response_instance.to_dict()
# create an instance of DeleteCertificateResponse from a dict
delete_certificate_response_from_dict = DeleteCertificateResponse.from_dict(delete_certificate_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


