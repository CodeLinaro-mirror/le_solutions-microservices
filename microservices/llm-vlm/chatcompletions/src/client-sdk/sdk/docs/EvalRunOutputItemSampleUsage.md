# EvalRunOutputItemSampleUsage

Token usage details for the sample.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**total_tokens** | **int** | The total number of tokens used. | 
**completion_tokens** | **int** | The number of completion tokens generated. | 
**prompt_tokens** | **int** | The number of prompt tokens used. | 
**cached_tokens** | **int** | The number of tokens retrieved from cache. | 

## Example

```python
from openapi_client.models.eval_run_output_item_sample_usage import EvalRunOutputItemSampleUsage

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItemSampleUsage from a JSON string
eval_run_output_item_sample_usage_instance = EvalRunOutputItemSampleUsage.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItemSampleUsage.to_json())

# convert the object into a dict
eval_run_output_item_sample_usage_dict = eval_run_output_item_sample_usage_instance.to_dict()
# create an instance of EvalRunOutputItemSampleUsage from a dict
eval_run_output_item_sample_usage_from_dict = EvalRunOutputItemSampleUsage.from_dict(eval_run_output_item_sample_usage_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


