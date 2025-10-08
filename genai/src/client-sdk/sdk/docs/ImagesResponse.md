# ImagesResponse

The response from the image generation endpoint.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**created** | **int** | The Unix timestamp (in seconds) of when the image was created. | 
**data** | [**List[Image]**](Image.md) | The list of generated images. | [optional] 
**usage** | [**ImagesResponseUsage**](ImagesResponseUsage.md) |  | [optional] 

## Example

```python
from openapi_client.models.images_response import ImagesResponse

# TODO update the JSON string below
json = "{}"
# create an instance of ImagesResponse from a JSON string
images_response_instance = ImagesResponse.from_json(json)
# print the JSON string representation of the object
print(ImagesResponse.to_json())

# convert the object into a dict
images_response_dict = images_response_instance.to_dict()
# create an instance of ImagesResponse from a dict
images_response_from_dict = ImagesResponse.from_dict(images_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


