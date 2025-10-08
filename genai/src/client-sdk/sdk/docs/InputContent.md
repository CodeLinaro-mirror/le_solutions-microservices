# InputContent


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_text&#x60;. | [default to 'input_text']
**text** | **str** | The text input to the model. | 
**image_url** | **str** | The URL of the image to be sent to the model. A fully qualified URL or base64 encoded image in a data URL. | [optional] 
**file_id** | **str** | The ID of the file to be sent to the model. | [optional] 
**detail** | **str** | The detail level of the image to be sent to the model. One of &#x60;high&#x60;, &#x60;low&#x60;, or &#x60;auto&#x60;. Defaults to &#x60;auto&#x60;. | 
**filename** | **str** | The name of the file to be sent to the model. | [optional] 
**file_data** | **str** | The content of the file to be sent to the model.  | [optional] 

## Example

```python
from openapi_client.models.input_content import InputContent

# TODO update the JSON string below
json = "{}"
# create an instance of InputContent from a JSON string
input_content_instance = InputContent.from_json(json)
# print the JSON string representation of the object
print(InputContent.to_json())

# convert the object into a dict
input_content_dict = input_content_instance.to_dict()
# create an instance of InputContent from a dict
input_content_from_dict = InputContent.from_dict(input_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


