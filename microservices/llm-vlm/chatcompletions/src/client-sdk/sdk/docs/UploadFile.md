# UploadFile


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The file identifier, which can be referenced in the API endpoints. | 
**bytes** | **int** | The size of the file, in bytes. | 
**created_at** | **int** | The Unix timestamp (in seconds) for when the file was created. | 
**expires_at** | **int** | The Unix timestamp (in seconds) for when the file will expire. | [optional] 
**filename** | **str** | The name of the file. | 
**object** | **str** | The object type, which is always &#x60;file&#x60;. | 
**purpose** | **str** | The intended purpose of the file. Supported values are &#x60;assistants&#x60;, &#x60;assistants_output&#x60;, &#x60;batch&#x60;, &#x60;batch_output&#x60;, &#x60;fine-tune&#x60;, &#x60;fine-tune-results&#x60; and &#x60;vision&#x60;. | 
**status** | **str** | Deprecated. The current status of the file, which can be either &#x60;uploaded&#x60;, &#x60;processed&#x60;, or &#x60;error&#x60;. | 
**status_details** | **str** | Deprecated. For details on why a fine-tuning training file failed validation, see the &#x60;error&#x60; field on &#x60;fine_tuning.job&#x60;. | [optional] 

## Example

```python
from openapi_client.models.upload_file import UploadFile

# TODO update the JSON string below
json = "{}"
# create an instance of UploadFile from a JSON string
upload_file_instance = UploadFile.from_json(json)
# print the JSON string representation of the object
print(UploadFile.to_json())

# convert the object into a dict
upload_file_dict = upload_file_instance.to_dict()
# create an instance of UploadFile from a dict
upload_file_from_dict = UploadFile.from_dict(upload_file_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


