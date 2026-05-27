# OutputText

A text output from the model. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the output text. Always &#x60;output_text&#x60;.  | 
**text** | **str** | The text output from the model.  | 

## Example

```python
from openapi_client.models.output_text import OutputText

# TODO update the JSON string below
json = "{}"
# create an instance of OutputText from a JSON string
output_text_instance = OutputText.from_json(json)
# print the JSON string representation of the object
print(OutputText.to_json())

# convert the object into a dict
output_text_dict = output_text_instance.to_dict()
# create an instance of OutputText from a dict
output_text_from_dict = OutputText.from_dict(output_text_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


