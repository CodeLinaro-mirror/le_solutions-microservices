# AddUploadPartRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**data** | **bytearray** | The chunk of bytes for this Part.  | 

## Example

```python
from openapi_client.models.add_upload_part_request import AddUploadPartRequest

# TODO update the JSON string below
json = "{}"
# create an instance of AddUploadPartRequest from a JSON string
add_upload_part_request_instance = AddUploadPartRequest.from_json(json)
# print the JSON string representation of the object
print(AddUploadPartRequest.to_json())

# convert the object into a dict
add_upload_part_request_dict = add_upload_part_request_instance.to_dict()
# create an instance of AddUploadPartRequest from a dict
add_upload_part_request_from_dict = AddUploadPartRequest.from_dict(add_upload_part_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


