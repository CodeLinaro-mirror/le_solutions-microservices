# InputImageContent

An image input to the model. Learn about [image inputs](/docs/guides/vision).

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_image&#x60;. | [default to 'input_image']
**image_url** | **str** | The URL of the image to be sent to the model. A fully qualified URL or base64 encoded image in a data URL. | [optional] 
**file_id** | **str** | The ID of the file to be sent to the model. | [optional] 
**detail** | **str** | The detail level of the image to be sent to the model. One of &#x60;high&#x60;, &#x60;low&#x60;, or &#x60;auto&#x60;. Defaults to &#x60;auto&#x60;. | 

## Example

```python
from openapi_client.models.input_image_content import InputImageContent

# TODO update the JSON string below
json = "{}"
# create an instance of InputImageContent from a JSON string
input_image_content_instance = InputImageContent.from_json(json)
# print the JSON string representation of the object
print(InputImageContent.to_json())

# convert the object into a dict
input_image_content_dict = input_image_content_instance.to_dict()
# create an instance of InputImageContent from a dict
input_image_content_from_dict = InputImageContent.from_dict(input_image_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


