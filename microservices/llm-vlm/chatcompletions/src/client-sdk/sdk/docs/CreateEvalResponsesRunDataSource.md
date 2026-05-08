# CreateEvalResponsesRunDataSource

A ResponsesRunDataSource object describing a model sampling configuration. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of run data source. Always &#x60;completions&#x60;. | [default to 'completions']
**input_messages** | [**CreateEvalResponsesRunDataSourceInputMessages**](CreateEvalResponsesRunDataSourceInputMessages.md) |  | [optional] 
**sampling_params** | [**CreateEvalCompletionsRunDataSourceSamplingParams**](CreateEvalCompletionsRunDataSourceSamplingParams.md) |  | [optional] 
**model** | **str** | The name of the model to use for generating completions (e.g. \&quot;o3-mini\&quot;). | [optional] 
**source** | [**CreateEvalResponsesRunDataSourceSource**](CreateEvalResponsesRunDataSourceSource.md) |  | 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source import CreateEvalResponsesRunDataSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSource from a JSON string
create_eval_responses_run_data_source_instance = CreateEvalResponsesRunDataSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSource.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_dict = create_eval_responses_run_data_source_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSource from a dict
create_eval_responses_run_data_source_from_dict = CreateEvalResponsesRunDataSource.from_dict(create_eval_responses_run_data_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


