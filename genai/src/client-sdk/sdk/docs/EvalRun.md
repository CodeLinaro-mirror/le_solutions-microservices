# EvalRun

A schema representing an evaluation run. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of the object. Always \&quot;eval.run\&quot;. | [default to 'eval.run']
**id** | **str** | Unique identifier for the evaluation run. | 
**eval_id** | **str** | The identifier of the associated evaluation. | 
**status** | **str** | The status of the evaluation run. | 
**model** | **str** | The model that is evaluated, if applicable. | 
**name** | **str** | The name of the evaluation run. | 
**created_at** | **int** | Unix timestamp (in seconds) when the evaluation run was created. | 
**report_url** | **str** | The URL to the rendered evaluation run report on the UI dashboard. | 
**result_counts** | [**EvalRunResultCounts**](EvalRunResultCounts.md) |  | 
**per_model_usage** | [**List[EvalRunPerModelUsageInner]**](EvalRunPerModelUsageInner.md) | Usage statistics for each model during the evaluation run. | 
**per_testing_criteria_results** | [**List[EvalRunPerTestingCriteriaResultsInner]**](EvalRunPerTestingCriteriaResultsInner.md) | Results per testing criteria applied during the evaluation run. | 
**data_source** | [**EvalRunDataSource**](EvalRunDataSource.md) |  | 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | 
**error** | [**EvalApiError**](EvalApiError.md) |  | 

## Example

```python
from openapi_client.models.eval_run import EvalRun

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRun from a JSON string
eval_run_instance = EvalRun.from_json(json)
# print the JSON string representation of the object
print(EvalRun.to_json())

# convert the object into a dict
eval_run_dict = eval_run_instance.to_dict()
# create an instance of EvalRun from a dict
eval_run_from_dict = EvalRun.from_dict(eval_run_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


