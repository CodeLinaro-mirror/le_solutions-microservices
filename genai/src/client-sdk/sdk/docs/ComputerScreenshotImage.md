# ComputerScreenshotImage

A computer screenshot image used with the computer use tool. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a computer screenshot, this property is  always set to &#x60;computer_screenshot&#x60;.  | [default to 'computer_screenshot']
**image_url** | **str** | The URL of the screenshot image. | [optional] 
**file_id** | **str** | The identifier of an uploaded file that contains the screenshot. | [optional] 

## Example

```python
from openapi_client.models.computer_screenshot_image import ComputerScreenshotImage

# TODO update the JSON string below
json = "{}"
# create an instance of ComputerScreenshotImage from a JSON string
computer_screenshot_image_instance = ComputerScreenshotImage.from_json(json)
# print the JSON string representation of the object
print(ComputerScreenshotImage.to_json())

# convert the object into a dict
computer_screenshot_image_dict = computer_screenshot_image_instance.to_dict()
# create an instance of ComputerScreenshotImage from a dict
computer_screenshot_image_from_dict = ComputerScreenshotImage.from_dict(computer_screenshot_image_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


