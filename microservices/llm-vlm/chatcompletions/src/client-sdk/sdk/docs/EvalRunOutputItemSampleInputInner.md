# EvalRunOutputItemSampleInputInner

An input message.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message sender (e.g., system, user, developer). | 
**content** | **str** | The content of the message. | 

## Example

```python
from openapi_client.models.eval_run_output_item_sample_input_inner import EvalRunOutputItemSampleInputInner

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItemSampleInputInner from a JSON string
eval_run_output_item_sample_input_inner_instance = EvalRunOutputItemSampleInputInner.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItemSampleInputInner.to_json())

# convert the object into a dict
eval_run_output_item_sample_input_inner_dict = eval_run_output_item_sample_input_inner_instance.to_dict()
# create an instance of EvalRunOutputItemSampleInputInner from a dict
eval_run_output_item_sample_input_inner_from_dict = EvalRunOutputItemSampleInputInner.from_dict(eval_run_output_item_sample_input_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


