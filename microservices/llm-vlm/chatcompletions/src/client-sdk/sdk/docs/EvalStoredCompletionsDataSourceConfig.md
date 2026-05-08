# EvalStoredCompletionsDataSourceConfig

A StoredCompletionsDataSourceConfig which specifies the metadata property of your stored completions query. This is usually metadata like `usecase=chatbot` or `prompt-version=v2`, etc. The schema returned by this data source config is used to defined what variables are available in your evals. `item` and `sample` are both defined when using this data source config. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;stored_completions&#x60;. | [default to 'stored_completions']
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**var_schema** | **Dict[str, object]** | The json schema for the run data source items.  | 

## Example

```python
from openapi_client.models.eval_stored_completions_data_source_config import EvalStoredCompletionsDataSourceConfig

# TODO update the JSON string below
json = "{}"
# create an instance of EvalStoredCompletionsDataSourceConfig from a JSON string
eval_stored_completions_data_source_config_instance = EvalStoredCompletionsDataSourceConfig.from_json(json)
# print the JSON string representation of the object
print(EvalStoredCompletionsDataSourceConfig.to_json())

# convert the object into a dict
eval_stored_completions_data_source_config_dict = eval_stored_completions_data_source_config_instance.to_dict()
# create an instance of EvalStoredCompletionsDataSourceConfig from a dict
eval_stored_completions_data_source_config_from_dict = EvalStoredCompletionsDataSourceConfig.from_dict(eval_stored_completions_data_source_config_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


