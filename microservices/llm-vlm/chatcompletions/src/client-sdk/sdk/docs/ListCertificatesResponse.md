# ListCertificatesResponse


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**data** | [**List[Certificate]**](Certificate.md) |  | 
**first_id** | **str** |  | [optional] 
**last_id** | **str** |  | [optional] 
**has_more** | **bool** |  | 
**object** | **str** |  | 

## Example

```python
from openapi_client.models.list_certificates_response import ListCertificatesResponse

# TODO update the JSON string below
json = "{}"
# create an instance of ListCertificatesResponse from a JSON string
list_certificates_response_instance = ListCertificatesResponse.from_json(json)
# print the JSON string representation of the object
print(ListCertificatesResponse.to_json())

# convert the object into a dict
list_certificates_response_dict = list_certificates_response_instance.to_dict()
# create an instance of ListCertificatesResponse from a dict
list_certificates_response_from_dict = ListCertificatesResponse.from_dict(list_certificates_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


