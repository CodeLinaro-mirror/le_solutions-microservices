# JSONSchema

Structured Outputs configuration options, including a JSON Schema. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**description** | **str** | A description of what the response format is for, used by the model to determine how to respond in the format.  | [optional] 
**name** | **str** | The name of the response format. Must be a-z, A-Z, 0-9, or contain underscores and dashes, with a maximum length of 64.  | 
**var_schema** | **Dict[str, object]** | The schema for the response format, described as a JSON Schema object.  | [optional] 
**strict** | **bool** | Whether to enable strict schema adherence when generating the output. If set to true, the model will always follow the exact schema defined in the &#x60;schema&#x60; field. Only a subset of JSON Schema is supported when &#x60;strict&#x60; is &#x60;true&#x60;. To learn more, read the [Structured Outputs guide](/docs/guides/structured-outputs).  | [optional] [default to False]

## Example

```python
from openapi_client.models.json_schema import JSONSchema

# TODO update the JSON string below
json = "{}"
# create an instance of JSONSchema from a JSON string
json_schema_instance = JSONSchema.from_json(json)
# print the JSON string representation of the object
print(JSONSchema.to_json())

# convert the object into a dict
json_schema_dict = json_schema_instance.to_dict()
# create an instance of JSONSchema from a dict
json_schema_from_dict = JSONSchema.from_dict(json_schema_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


