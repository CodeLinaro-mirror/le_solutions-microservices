# Content

Multi-modal input and output contents. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_file&#x60;. | [default to 'input_file']
**text** | **str** | The text output from the model. | 
**image_url** | **str** | The URL of the image to be sent to the model. A fully qualified URL or base64 encoded image in a data URL. | [optional] 
**file_id** | **str** | The ID of the file to be sent to the model. | [optional] 
**detail** | **str** | The detail level of the image to be sent to the model. One of &#x60;high&#x60;, &#x60;low&#x60;, or &#x60;auto&#x60;. Defaults to &#x60;auto&#x60;. | 
**filename** | **str** | The name of the file to be sent to the model. | [optional] 
**file_data** | **str** | The content of the file to be sent to the model.  | [optional] 
**annotations** | [**List[Annotation]**](Annotation.md) | The annotations of the text output. | 
**refusal** | **str** | The refusal explanationfrom the model. | 

## Example

```python
from openapi_client.models.content import Content

# TODO update the JSON string below
json = "{}"
# create an instance of Content from a JSON string
content_instance = Content.from_json(json)
# print the JSON string representation of the object
print(Content.to_json())

# convert the object into a dict
content_dict = content_instance.to_dict()
# create an instance of Content from a dict
content_from_dict = Content.from_dict(content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


