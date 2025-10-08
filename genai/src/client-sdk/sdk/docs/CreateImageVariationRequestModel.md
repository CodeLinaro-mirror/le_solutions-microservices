# CreateImageVariationRequestModel

The model to use for image generation. Only `dall-e-2` is supported at this time.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.create_image_variation_request_model import CreateImageVariationRequestModel

# TODO update the JSON string below
json = "{}"
# create an instance of CreateImageVariationRequestModel from a JSON string
create_image_variation_request_model_instance = CreateImageVariationRequestModel.from_json(json)
# print the JSON string representation of the object
print(CreateImageVariationRequestModel.to_json())

# convert the object into a dict
create_image_variation_request_model_dict = create_image_variation_request_model_instance.to_dict()
# create an instance of CreateImageVariationRequestModel from a dict
create_image_variation_request_model_from_dict = CreateImageVariationRequestModel.from_dict(create_image_variation_request_model_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


