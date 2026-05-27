# EvalRunOutputItemSampleOutputInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message (e.g. \&quot;system\&quot;, \&quot;assistant\&quot;, \&quot;user\&quot;). | [optional] 
**content** | **str** | The content of the message. | [optional] 

## Example

```python
from openapi_client.models.eval_run_output_item_sample_output_inner import EvalRunOutputItemSampleOutputInner

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItemSampleOutputInner from a JSON string
eval_run_output_item_sample_output_inner_instance = EvalRunOutputItemSampleOutputInner.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItemSampleOutputInner.to_json())

# convert the object into a dict
eval_run_output_item_sample_output_inner_dict = eval_run_output_item_sample_output_inner_instance.to_dict()
# create an instance of EvalRunOutputItemSampleOutputInner from a dict
eval_run_output_item_sample_output_inner_from_dict = EvalRunOutputItemSampleOutputInner.from_dict(eval_run_output_item_sample_output_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


