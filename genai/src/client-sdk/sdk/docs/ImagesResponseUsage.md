# ImagesResponseUsage

For `llama-3.1-8` only, the token usage information for the image generation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**total_tokens** | **int** | The total number of tokens (images and text) used for the image generation. | 
**input_tokens** | **int** | The number of tokens (images and text) in the input prompt. | 
**output_tokens** | **int** | The number of image tokens in the output image. | 
**input_tokens_details** | [**ImagesResponseUsageInputTokensDetails**](ImagesResponseUsageInputTokensDetails.md) |  | 

## Example

```python
from openapi_client.models.images_response_usage import ImagesResponseUsage

# TODO update the JSON string below
json = "{}"
# create an instance of ImagesResponseUsage from a JSON string
images_response_usage_instance = ImagesResponseUsage.from_json(json)
# print the JSON string representation of the object
print(ImagesResponseUsage.to_json())

# convert the object into a dict
images_response_usage_dict = images_response_usage_instance.to_dict()
# create an instance of ImagesResponseUsage from a dict
images_response_usage_from_dict = ImagesResponseUsage.from_dict(images_response_usage_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


