# OutputTextContent

A text output from the model.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the output text. Always &#x60;output_text&#x60;. | [default to 'output_text']
**text** | **str** | The text output from the model. | 
**annotations** | [**List[Annotation]**](Annotation.md) | The annotations of the text output. | 

## Example

```python
from openapi_client.models.output_text_content import OutputTextContent

# TODO update the JSON string below
json = "{}"
# create an instance of OutputTextContent from a JSON string
output_text_content_instance = OutputTextContent.from_json(json)
# print the JSON string representation of the object
print(OutputTextContent.to_json())

# convert the object into a dict
output_text_content_dict = output_text_content_instance.to_dict()
# create an instance of OutputTextContent from a dict
output_text_content_from_dict = OutputTextContent.from_dict(output_text_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


