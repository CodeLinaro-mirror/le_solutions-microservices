# EvalRunPerTestingCriteriaResultsInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**testing_criteria** | **str** | A description of the testing criteria. | 
**passed** | **int** | Number of tests passed for this criteria. | 
**failed** | **int** | Number of tests failed for this criteria. | 

## Example

```python
from openapi_client.models.eval_run_per_testing_criteria_results_inner import EvalRunPerTestingCriteriaResultsInner

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunPerTestingCriteriaResultsInner from a JSON string
eval_run_per_testing_criteria_results_inner_instance = EvalRunPerTestingCriteriaResultsInner.from_json(json)
# print the JSON string representation of the object
print(EvalRunPerTestingCriteriaResultsInner.to_json())

# convert the object into a dict
eval_run_per_testing_criteria_results_inner_dict = eval_run_per_testing_criteria_results_inner_instance.to_dict()
# create an instance of EvalRunPerTestingCriteriaResultsInner from a dict
eval_run_per_testing_criteria_results_inner_from_dict = EvalRunPerTestingCriteriaResultsInner.from_dict(eval_run_per_testing_criteria_results_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


