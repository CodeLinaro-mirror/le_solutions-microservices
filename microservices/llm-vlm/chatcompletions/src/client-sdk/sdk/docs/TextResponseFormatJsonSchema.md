# TextResponseFormatJsonSchema

JSON Schema response format. Used to generate structured JSON responses. Learn more about [Structured Outputs](/docs/guides/structured-outputs). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of response format being defined. Always &#x60;json_schema&#x60;. | 
**description** | **str** | A description of what the response format is for, used by the model to determine how to respond in the format.  | [optional] 
**name** | **str** | The name of the response format. Must be a-z, A-Z, 0-9, or contain underscores and dashes, with a maximum length of 64.  | 
**var_schema** | **Dict[str, object]** | The schema for the response format, described as a JSON Schema object.  | 
**strict** | **bool** | Whether to enable strict schema adherence when generating the output. If set to true, the model will always follow the exact schema defined in the &#x60;schema&#x60; field. Only a subset of JSON Schema is supported when &#x60;strict&#x60; is &#x60;true&#x60;. To learn more, read the [Structured Outputs guide](/docs/guides/structured-outputs).  | [optional] [default to False]

## Example

```python
from openapi_client.models.text_response_format_json_schema import TextResponseFormatJsonSchema

# TODO update the JSON string below
json = "{}"
# create an instance of TextResponseFormatJsonSchema from a JSON string
text_response_format_json_schema_instance = TextResponseFormatJsonSchema.from_json(json)
# print the JSON string representation of the object
print(TextResponseFormatJsonSchema.to_json())

# convert the object into a dict
text_response_format_json_schema_dict = text_response_format_json_schema_instance.to_dict()
# create an instance of TextResponseFormatJsonSchema from a dict
text_response_format_json_schema_from_dict = TextResponseFormatJsonSchema.from_dict(text_response_format_json_schema_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


