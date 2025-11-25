# EvalRunDataSource

Information about the run's data source.

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
from openapi_client.models.eval_run_data_source import EvalRunDataSource

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunDataSource from a JSON string
eval_run_data_source_instance = EvalRunDataSource.from_json(json)
# print the JSON string representation of the object
print(EvalRunDataSource.to_json())

# convert the object into a dict
eval_run_data_source_dict = eval_run_data_source_instance.to_dict()
# create an instance of EvalRunDataSource from a dict
eval_run_data_source_from_dict = EvalRunDataSource.from_dict(eval_run_data_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


