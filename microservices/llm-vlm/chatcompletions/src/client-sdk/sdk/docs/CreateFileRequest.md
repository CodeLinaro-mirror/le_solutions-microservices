# CreateFileRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file** | **bytearray** | The File object (not file name) to be uploaded.  | 
**purpose** | **str** | The intended purpose of the uploaded file. One of: - &#x60;assistants&#x60;: Used in the Assistants API - &#x60;batch&#x60;: Used in the Batch API - &#x60;fine-tune&#x60;: Used for fine-tuning - &#x60;vision&#x60;: Images used for vision fine-tuning - &#x60;user_data&#x60;: Flexible file type for any purpose - &#x60;evals&#x60;: Used for eval data sets  | 

## Example

```python
from openapi_client.models.create_file_request import CreateFileRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateFileRequest from a JSON string
create_file_request_instance = CreateFileRequest.from_json(json)
# print the JSON string representation of the object
print(CreateFileRequest.to_json())

# convert the object into a dict
create_file_request_dict = create_file_request_instance.to_dict()
# create an instance of CreateFileRequest from a dict
create_file_request_from_dict = CreateFileRequest.from_dict(create_file_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


