# ResponsePropertiesText

Configuration options for a text response from the model. Can be plain text or structured JSON data. Learn more: - [Text inputs and outputs](/docs/guides/text) - [Structured Outputs](/docs/guides/structured-outputs) 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**format** | [**TextResponseFormatConfiguration**](TextResponseFormatConfiguration.md) |  | [optional] 

## Example

```python
from openapi_client.models.response_properties_text import ResponsePropertiesText

# TODO update the JSON string below
json = "{}"
# create an instance of ResponsePropertiesText from a JSON string
response_properties_text_instance = ResponsePropertiesText.from_json(json)
# print the JSON string representation of the object
print(ResponsePropertiesText.to_json())

# convert the object into a dict
response_properties_text_dict = response_properties_text_instance.to_dict()
# create an instance of ResponsePropertiesText from a dict
response_properties_text_from_dict = ResponsePropertiesText.from_dict(response_properties_text_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


