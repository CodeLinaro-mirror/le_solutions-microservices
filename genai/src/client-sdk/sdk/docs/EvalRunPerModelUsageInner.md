# EvalRunPerModelUsageInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**model_name** | **str** | The name of the model. | 
**invocation_count** | **int** | The number of invocations. | 
**prompt_tokens** | **int** | The number of prompt tokens used. | 
**completion_tokens** | **int** | The number of completion tokens generated. | 
**total_tokens** | **int** | The total number of tokens used. | 
**cached_tokens** | **int** | The number of tokens retrieved from cache. | 

## Example

```python
from openapi_client.models.eval_run_per_model_usage_inner import EvalRunPerModelUsageInner

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunPerModelUsageInner from a JSON string
eval_run_per_model_usage_inner_instance = EvalRunPerModelUsageInner.from_json(json)
# print the JSON string representation of the object
print(EvalRunPerModelUsageInner.to_json())

# convert the object into a dict
eval_run_per_model_usage_inner_dict = eval_run_per_model_usage_inner_instance.to_dict()
# create an instance of EvalRunPerModelUsageInner from a dict
eval_run_per_model_usage_inner_from_dict = EvalRunPerModelUsageInner.from_dict(eval_run_per_model_usage_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


