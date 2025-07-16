# EvalCustomDataSourceConfig

A CustomDataSourceConfig which specifies the schema of your `item` and optionally `sample` namespaces. The response schema defines the shape of the data that will be: - Used to define your testing criteria and - What data is required when creating a run 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;custom&#x60;. | [default to 'custom']
**var_schema** | **Dict[str, object]** | The json schema for the run data source items.  | 

## Example

```python
from openapi_client.models.eval_custom_data_source_config import EvalCustomDataSourceConfig

# TODO update the JSON string below
json = "{}"
# create an instance of EvalCustomDataSourceConfig from a JSON string
eval_custom_data_source_config_instance = EvalCustomDataSourceConfig.from_json(json)
# print the JSON string representation of the object
print(EvalCustomDataSourceConfig.to_json())

# convert the object into a dict
eval_custom_data_source_config_dict = eval_custom_data_source_config_instance.to_dict()
# create an instance of EvalCustomDataSourceConfig from a dict
eval_custom_data_source_config_from_dict = EvalCustomDataSourceConfig.from_dict(eval_custom_data_source_config_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


