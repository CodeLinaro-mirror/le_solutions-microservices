# EvalRunOutputItemSample

A sample containing the input and output of the evaluation run.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**input** | [**List[EvalRunOutputItemSampleInputInner]**](EvalRunOutputItemSampleInputInner.md) | An array of input messages. | 
**output** | [**List[EvalRunOutputItemSampleOutputInner]**](EvalRunOutputItemSampleOutputInner.md) | An array of output messages. | 
**finish_reason** | **str** | The reason why the sample generation was finished. | 
**model** | **str** | The model used for generating the sample. | 
**usage** | [**EvalRunOutputItemSampleUsage**](EvalRunOutputItemSampleUsage.md) |  | 
**error** | [**EvalApiError**](EvalApiError.md) |  | 
**temperature** | **float** | The sampling temperature used. | 
**max_completion_tokens** | **int** | The maximum number of tokens allowed for completion. | 
**top_p** | **float** | The top_p value used for sampling. | 
**seed** | **int** | The seed used for generating the sample. | 

## Example

```python
from openapi_client.models.eval_run_output_item_sample import EvalRunOutputItemSample

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItemSample from a JSON string
eval_run_output_item_sample_instance = EvalRunOutputItemSample.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItemSample.to_json())

# convert the object into a dict
eval_run_output_item_sample_dict = eval_run_output_item_sample_instance.to_dict()
# create an instance of EvalRunOutputItemSample from a dict
eval_run_output_item_sample_from_dict = EvalRunOutputItemSample.from_dict(eval_run_output_item_sample_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


