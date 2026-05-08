# CreateEvalRequestDataSourceConfig

The configuration for the data source used for the evaluation runs.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;custom&#x60;. | [default to 'custom']
**item_schema** | **Dict[str, object]** | The json schema for each row in the data source. | 
**include_sample_schema** | **bool** | Whether the eval should expect you to populate the sample namespace (ie, by generating responses off of your data source) | [optional] [default to False]
**metadata** | **Dict[str, object]** | Metadata filters for the logs data source. | [optional] 

## Example

```python
from openapi_client.models.create_eval_request_data_source_config import CreateEvalRequestDataSourceConfig

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalRequestDataSourceConfig from a JSON string
create_eval_request_data_source_config_instance = CreateEvalRequestDataSourceConfig.from_json(json)
# print the JSON string representation of the object
print(CreateEvalRequestDataSourceConfig.to_json())

# convert the object into a dict
create_eval_request_data_source_config_dict = create_eval_request_data_source_config_instance.to_dict()
# create an instance of CreateEvalRequestDataSourceConfig from a dict
create_eval_request_data_source_config_from_dict = CreateEvalRequestDataSourceConfig.from_dict(create_eval_request_data_source_config_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


