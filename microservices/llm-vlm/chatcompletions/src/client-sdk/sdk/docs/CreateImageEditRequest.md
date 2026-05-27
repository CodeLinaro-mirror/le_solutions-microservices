# CreateImageEditRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**image** | [**CreateImageEditRequestImage**](CreateImageEditRequestImage.md) |  | 
**prompt** | **str** | A text description of the desired image(s). | 
**mask** | **bytearray** | An additional image whose fully transparent areas (e.g. where alpha is zero) indicate where &#x60;image&#x60; should be edited. If there are multiple images provided, the mask will be applied on the first image. Must be a valid PNG file, less than 4MB, and have the same dimensions as &#x60;image&#x60;. | [optional] 
**n** | **int** | The number of images to generate. Must be between 1 and 10. | [optional] [default to 1]
**size** | **str** | The size of the generated images. Must be one of &#x60;1024x1024&#x60;, &#x60;1536x1024&#x60; (landscape), &#x60;1024x1536&#x60; (portrait), or &#x60;auto&#x60; (default value). | [optional] [default to '1024x1024']
**response_format** | **str** | The format in which the generated images are returned. Must be one of &#x60;url&#x60; or &#x60;b64_json&#x60;. URLs are only valid for 60 minutes after the image has been generated. This parameter will always return base64-encoded images. | [optional] [default to 'url']
**user** | **str** | A unique identifier representing your end-user, which can help GenAI to monitor and detect abuse. [Learn more](/docs/guides/safety-best-practices#end-user-ids).  | [optional] 
**quality** | **str** | The quality of the image that will be generated. &#x60;high&#x60;, &#x60;medium&#x60; and &#x60;low&#x60; are only supported as per model only supports &#x60;standard&#x60; quality. Defaults to &#x60;auto&#x60;.  | [optional] [default to 'auto']

## Example

```python
from openapi_client.models.create_image_edit_request import CreateImageEditRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateImageEditRequest from a JSON string
create_image_edit_request_instance = CreateImageEditRequest.from_json(json)
# print the JSON string representation of the object
print(CreateImageEditRequest.to_json())

# convert the object into a dict
create_image_edit_request_dict = create_image_edit_request_instance.to_dict()
# create an instance of CreateImageEditRequest from a dict
create_image_edit_request_from_dict = CreateImageEditRequest.from_dict(create_image_edit_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


