# OutputContent


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the output text. Always &#x60;output_text&#x60;. | [default to 'output_text']
**text** | **str** | The text output from the model. | 
**annotations** | [**List[Annotation]**](Annotation.md) | The annotations of the text output. | 
**refusal** | **str** | The refusal explanationfrom the model. | 

## Example

```python
from openapi_client.models.output_content import OutputContent

# TODO update the JSON string below
json = "{}"
# create an instance of OutputContent from a JSON string
output_content_instance = OutputContent.from_json(json)
# print the JSON string representation of the object
print(OutputContent.to_json())

# convert the object into a dict
output_content_dict = output_content_instance.to_dict()
# create an instance of OutputContent from a dict
output_content_from_dict = OutputContent.from_dict(output_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


