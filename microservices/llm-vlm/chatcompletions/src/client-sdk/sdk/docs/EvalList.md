# EvalList

An object representing a list of evals. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of this object. It is always set to \&quot;list\&quot;.  | [default to 'list']
**data** | [**List[Eval]**](Eval.md) | An array of eval objects.  | 
**first_id** | **str** | The identifier of the first eval in the data array. | 
**last_id** | **str** | The identifier of the last eval in the data array. | 
**has_more** | **bool** | Indicates whether there are more evals available. | 

## Example

```python
from openapi_client.models.eval_list import EvalList

# TODO update the JSON string below
json = "{}"
# create an instance of EvalList from a JSON string
eval_list_instance = EvalList.from_json(json)
# print the JSON string representation of the object
print(EvalList.to_json())

# convert the object into a dict
eval_list_dict = eval_list_instance.to_dict()
# create an instance of EvalList from a dict
eval_list_from_dict = EvalList.from_dict(eval_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


