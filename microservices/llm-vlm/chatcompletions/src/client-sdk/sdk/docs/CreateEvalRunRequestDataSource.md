# CreateEvalRunRequestDataSource

Details about the run's data source.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;jsonl&#x60;. | [default to 'jsonl']
**source** | [**CreateEvalResponsesRunDataSourceSource**](CreateEvalResponsesRunDataSourceSource.md) |  | 
**input_messages** | [**CreateEvalResponsesRunDataSourceInputMessages**](CreateEvalResponsesRunDataSourceInputMessages.md) |  | [optional] 
**sampling_params** | [**CreateEvalCompletionsRunDataSourceSamplingParams**](CreateEvalCompletionsRunDataSourceSamplingParams.md) |  | [optional] 
**model** | **str** | The name of the model to use for generating completions (e.g. \&quot;o3-mini\&quot;). | [optional] 

## Example

```python
from openapi_client.models.create_eval_run_request_data_source import CreateEvalRunRequestDataSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalRunRequestDataSource from a JSON string
create_eval_run_request_data_source_instance = CreateEvalRunRequestDataSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalRunRequestDataSource.to_json())

# convert the object into a dict
create_eval_run_request_data_source_dict = create_eval_run_request_data_source_instance.to_dict()
# create an instance of CreateEvalRunRequestDataSource from a dict
create_eval_run_request_data_source_from_dict = CreateEvalRunRequestDataSource.from_dict(create_eval_run_request_data_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


