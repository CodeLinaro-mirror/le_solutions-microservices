# InputTextContent

A text input to the model.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the input item. Always &#x60;input_text&#x60;. | [default to 'input_text']
**text** | **str** | The text input to the model. | 

## Example

```python
from openapi_client.models.input_text_content import InputTextContent

# TODO update the JSON string below
json = "{}"
# create an instance of InputTextContent from a JSON string
input_text_content_instance = InputTextContent.from_json(json)
# print the JSON string representation of the object
print(InputTextContent.to_json())

# convert the object into a dict
input_text_content_dict = input_text_content_instance.to_dict()
# create an instance of InputTextContent from a dict
input_text_content_from_dict = InputTextContent.from_dict(input_text_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


