# EvalRunOutputItem

A schema representing an evaluation run output item. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of the object. Always \&quot;eval.run.output_item\&quot;. | [default to 'eval.run.output_item']
**id** | **str** | Unique identifier for the evaluation run output item. | 
**run_id** | **str** | The identifier of the evaluation run associated with this output item. | 
**eval_id** | **str** | The identifier of the evaluation group. | 
**created_at** | **int** | Unix timestamp (in seconds) when the evaluation run was created. | 
**status** | **str** | The status of the evaluation run. | 
**datasource_item_id** | **int** | The identifier for the data source item. | 
**datasource_item** | **Dict[str, object]** | Details of the input data source item. | 
**results** | **List[Dict[str, object]]** | A list of results from the evaluation run. | 
**sample** | [**EvalRunOutputItemSample**](EvalRunOutputItemSample.md) |  | 

## Example

```python
from openapi_client.models.eval_run_output_item import EvalRunOutputItem

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItem from a JSON string
eval_run_output_item_instance = EvalRunOutputItem.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItem.to_json())

# convert the object into a dict
eval_run_output_item_dict = eval_run_output_item_instance.to_dict()
# create an instance of EvalRunOutputItem from a dict
eval_run_output_item_from_dict = EvalRunOutputItem.from_dict(eval_run_output_item_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


