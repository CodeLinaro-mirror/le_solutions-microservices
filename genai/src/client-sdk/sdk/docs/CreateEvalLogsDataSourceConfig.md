# CreateEvalLogsDataSourceConfig

A data source config which specifies the metadata property of your stored completions query. This is usually metadata like `usecase=chatbot` or `prompt-version=v2`, etc. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;logs&#x60;. | [default to 'logs']
**metadata** | **Dict[str, object]** | Metadata filters for the logs data source. | [optional] 

## Example

```python
from openapi_client.models.create_eval_logs_data_source_config import CreateEvalLogsDataSourceConfig

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalLogsDataSourceConfig from a JSON string
create_eval_logs_data_source_config_instance = CreateEvalLogsDataSourceConfig.from_json(json)
# print the JSON string representation of the object
print(CreateEvalLogsDataSourceConfig.to_json())

# convert the object into a dict
create_eval_logs_data_source_config_dict = create_eval_logs_data_source_config_instance.to_dict()
# create an instance of CreateEvalLogsDataSourceConfig from a dict
create_eval_logs_data_source_config_from_dict = CreateEvalLogsDataSourceConfig.from_dict(create_eval_logs_data_source_config_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


