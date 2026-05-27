# EvalDataSourceConfig

Configuration of data sources used in runs of the evaluation.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;custom&#x60;. | [default to 'custom']
**var_schema** | **Dict[str, object]** | The json schema for the run data source items.  | 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 

## Example

```python
from openapi_client.models.eval_data_source_config import EvalDataSourceConfig

# TODO update the JSON string below
json = "{}"
# create an instance of EvalDataSourceConfig from a JSON string
eval_data_source_config_instance = EvalDataSourceConfig.from_json(json)
# print the JSON string representation of the object
print(EvalDataSourceConfig.to_json())

# convert the object into a dict
eval_data_source_config_dict = eval_data_source_config_instance.to_dict()
# create an instance of EvalDataSourceConfig from a dict
eval_data_source_config_from_dict = EvalDataSourceConfig.from_dict(eval_data_source_config_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


