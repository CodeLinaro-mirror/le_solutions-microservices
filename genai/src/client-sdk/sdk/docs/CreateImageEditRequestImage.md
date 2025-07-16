# CreateImageEditRequestImage

The image(s) to edit. Must be a supported image file or an array of images.  Each image should be a `png`, `webp`, or `jpg` file less  than 25MB. You can provide up to 16 images.  For `dall-e-2`, you can only provide one image, and it should be a square  `png` file less than 4MB. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------

## Example

```python
from openapi_client.models.create_image_edit_request_image import CreateImageEditRequestImage

# TODO update the JSON string below
json = "{}"
# create an instance of CreateImageEditRequestImage from a JSON string
create_image_edit_request_image_instance = CreateImageEditRequestImage.from_json(json)
# print the JSON string representation of the object
print(CreateImageEditRequestImage.to_json())

# convert the object into a dict
create_image_edit_request_image_dict = create_image_edit_request_image_instance.to_dict()
# create an instance of CreateImageEditRequestImage from a dict
create_image_edit_request_image_from_dict = CreateImageEditRequestImage.from_dict(create_image_edit_request_image_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


