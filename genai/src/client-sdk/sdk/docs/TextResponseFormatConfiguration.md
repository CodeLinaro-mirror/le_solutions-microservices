# TextResponseFormatConfiguration

An object specifying the format that the model must output.  Configuring `{ \"type\": \"json_schema\" }` enables Structured Outputs,  which ensures the model will match your supplied JSON schema. Learn more in the  [Structured Outputs guide](/docs/guides/structured-outputs).  The default format is `{ \"type\": \"text\" }` with no additional options.  **Not recommended for newer models:**  Setting to `{ \"type\": \"json_object\" }` enables the older JSON mode, which ensures the message the model generates is valid JSON. Using `json_schema` is preferred for models that support it. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of response format being defined. Always &#x60;text&#x60;. | 
**description** | **str** | A description of what the response format is for, used by the model to determine how to respond in the format.  | [optional] 
**name** | **str** | The name of the response format. Must be a-z, A-Z, 0-9, or contain underscores and dashes, with a maximum length of 64.  | 
**var_schema** | **Dict[str, object]** | The schema for the response format, described as a JSON Schema object.  | 
**strict** | **bool** | Whether to enable strict schema adherence when generating the output. If set to true, the model will always follow the exact schema defined in the &#x60;schema&#x60; field. Only a subset of JSON Schema is supported when &#x60;strict&#x60; is &#x60;true&#x60;. To learn more, read the [Structured Outputs guide](/docs/guides/structured-outputs).  | [optional] [default to False]

## Example

```python
from openapi_client.models.text_response_format_configuration import TextResponseFormatConfiguration

# TODO update the JSON string below
json = "{}"
# create an instance of TextResponseFormatConfiguration from a JSON string
text_response_format_configuration_instance = TextResponseFormatConfiguration.from_json(json)
# print the JSON string representation of the object
print(TextResponseFormatConfiguration.to_json())

# convert the object into a dict
text_response_format_configuration_dict = text_response_format_configuration_instance.to_dict()
# create an instance of TextResponseFormatConfiguration from a dict
text_response_format_configuration_from_dict = TextResponseFormatConfiguration.from_dict(text_response_format_configuration_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


