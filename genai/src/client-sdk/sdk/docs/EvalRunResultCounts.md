# EvalRunResultCounts

Counters summarizing the outcomes of the evaluation run.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**total** | **int** | Total number of executed output items. | 
**errored** | **int** | Number of output items that resulted in an error. | 
**failed** | **int** | Number of output items that failed to pass the evaluation. | 
**passed** | **int** | Number of output items that passed the evaluation. | 

## Example

```python
from openapi_client.models.eval_run_result_counts import EvalRunResultCounts

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunResultCounts from a JSON string
eval_run_result_counts_instance = EvalRunResultCounts.from_json(json)
# print the JSON string representation of the object
print(EvalRunResultCounts.to_json())

# convert the object into a dict
eval_run_result_counts_dict = eval_run_result_counts_instance.to_dict()
# create an instance of EvalRunResultCounts from a dict
eval_run_result_counts_from_dict = EvalRunResultCounts.from_dict(eval_run_result_counts_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


