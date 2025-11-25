# CreateImageVariationRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**image** | **bytearray** | The image to use as the basis for the variation(s). Must be a valid PNG file, less than 4MB, and square. | 
**model** | [**CreateImageVariationRequestModel**](CreateImageVariationRequestModel.md) |  | [optional] 
**n** | **int** | The number of images to generate. Must be between 1 and 10. | [optional] [default to 1]
**response_format** | **str** | The format in which the generated images are returned. Must be one of &#x60;url&#x60; or &#x60;b64_json&#x60;. URLs are only valid for 60 minutes after the image has been generated. | [optional] [default to 'url']
**size** | **str** | The size of the generated images. Must be one of &#x60;256x256&#x60;, &#x60;512x512&#x60;, or &#x60;1024x1024&#x60;. | [optional] [default to '1024x1024']
**user** | **str** | A unique identifier representing your end-user, which can help GenAI to monitor and detect abuse. [Learn more](/docs/guides/safety-best-practices#end-user-ids).  | [optional] 

## Example

```python
from openapi_client.models.create_image_variation_request import CreateImageVariationRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateImageVariationRequest from a JSON string
create_image_variation_request_instance = CreateImageVariationRequest.from_json(json)
# print the JSON string representation of the object
print(CreateImageVariationRequest.to_json())

# convert the object into a dict
create_image_variation_request_dict = create_image_variation_request_instance.to_dict()
# create an instance of CreateImageVariationRequest from a dict
create_image_variation_request_from_dict = CreateImageVariationRequest.from_dict(create_image_variation_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


