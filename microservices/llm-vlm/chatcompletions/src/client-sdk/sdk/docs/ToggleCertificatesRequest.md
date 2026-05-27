# ToggleCertificatesRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**certificate_ids** | **List[str]** |  | 

## Example

```python
from openapi_client.models.toggle_certificates_request import ToggleCertificatesRequest

# TODO update the JSON string below
json = "{}"
# create an instance of ToggleCertificatesRequest from a JSON string
toggle_certificates_request_instance = ToggleCertificatesRequest.from_json(json)
# print the JSON string representation of the object
print(ToggleCertificatesRequest.to_json())

# convert the object into a dict
toggle_certificates_request_dict = toggle_certificates_request_instance.to_dict()
# create an instance of ToggleCertificatesRequest from a dict
toggle_certificates_request_from_dict = ToggleCertificatesRequest.from_dict(toggle_certificates_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


