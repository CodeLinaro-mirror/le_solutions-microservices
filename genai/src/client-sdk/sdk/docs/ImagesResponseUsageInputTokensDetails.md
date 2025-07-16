# ImagesResponseUsageInputTokensDetails

The input tokens detailed information for the image generation.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**text_tokens** | **int** | The number of text tokens in the input prompt. | 
**image_tokens** | **int** | The number of image tokens in the input prompt. | 

## Example

```python
from openapi_client.models.images_response_usage_input_tokens_details import ImagesResponseUsageInputTokensDetails

# TODO update the JSON string below
json = "{}"
# create an instance of ImagesResponseUsageInputTokensDetails from a JSON string
images_response_usage_input_tokens_details_instance = ImagesResponseUsageInputTokensDetails.from_json(json)
# print the JSON string representation of the object
print(ImagesResponseUsageInputTokensDetails.to_json())

# convert the object into a dict
images_response_usage_input_tokens_details_dict = images_response_usage_input_tokens_details_instance.to_dict()
# create an instance of ImagesResponseUsageInputTokensDetails from a dict
images_response_usage_input_tokens_details_from_dict = ImagesResponseUsageInputTokensDetails.from_dict(images_response_usage_input_tokens_details_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


