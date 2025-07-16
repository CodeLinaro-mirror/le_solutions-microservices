# CreateEvalCompletionsRunDataSourceSamplingParams


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**temperature** | **float** | A higher temperature increases randomness in the outputs. | [optional] [default to 1]
**max_completion_tokens** | **int** | The maximum number of tokens in the generated output. | [optional] 
**top_p** | **float** | An alternative to temperature for nucleus sampling; 1.0 includes all tokens. | [optional] [default to 1]
**seed** | **int** | A seed value to initialize the randomness, during sampling. | [optional] [default to 42]

## Example

```python
from openapi_client.models.create_eval_completions_run_data_source_sampling_params import CreateEvalCompletionsRunDataSourceSamplingParams

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalCompletionsRunDataSourceSamplingParams from a JSON string
create_eval_completions_run_data_source_sampling_params_instance = CreateEvalCompletionsRunDataSourceSamplingParams.from_json(json)
# print the JSON string representation of the object
print(CreateEvalCompletionsRunDataSourceSamplingParams.to_json())

# convert the object into a dict
create_eval_completions_run_data_source_sampling_params_dict = create_eval_completions_run_data_source_sampling_params_instance.to_dict()
# create an instance of CreateEvalCompletionsRunDataSourceSamplingParams from a dict
create_eval_completions_run_data_source_sampling_params_from_dict = CreateEvalCompletionsRunDataSourceSamplingParams.from_dict(create_eval_completions_run_data_source_sampling_params_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


