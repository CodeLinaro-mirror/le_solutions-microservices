# ModifyCertificateRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**name** | **str** | The updated name for the certificate | 

## Example

```python
from openapi_client.models.modify_certificate_request import ModifyCertificateRequest

# TODO update the JSON string below
json = "{}"
# create an instance of ModifyCertificateRequest from a JSON string
modify_certificate_request_instance = ModifyCertificateRequest.from_json(json)
# print the JSON string representation of the object
print(ModifyCertificateRequest.to_json())

# convert the object into a dict
modify_certificate_request_dict = modify_certificate_request_instance.to_dict()
# create an instance of ModifyCertificateRequest from a dict
modify_certificate_request_from_dict = ModifyCertificateRequest.from_dict(modify_certificate_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


