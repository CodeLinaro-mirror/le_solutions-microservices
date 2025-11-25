# EvalRunOutputItemList

An object representing a list of output items for an evaluation run. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of this object. It is always set to \&quot;list\&quot;.  | [default to 'list']
**data** | [**List[EvalRunOutputItem]**](EvalRunOutputItem.md) | An array of eval run output item objects.  | 
**first_id** | **str** | The identifier of the first eval run output item in the data array. | 
**last_id** | **str** | The identifier of the last eval run output item in the data array. | 
**has_more** | **bool** | Indicates whether there are more eval run output items available. | 

## Example

```python
from openapi_client.models.eval_run_output_item_list import EvalRunOutputItemList

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunOutputItemList from a JSON string
eval_run_output_item_list_instance = EvalRunOutputItemList.from_json(json)
# print the JSON string representation of the object
print(EvalRunOutputItemList.to_json())

# convert the object into a dict
eval_run_output_item_list_dict = eval_run_output_item_list_instance.to_dict()
# create an instance of EvalRunOutputItemList from a dict
eval_run_output_item_list_from_dict = EvalRunOutputItemList.from_dict(eval_run_output_item_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


