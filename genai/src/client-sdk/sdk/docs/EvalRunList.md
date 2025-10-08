# EvalRunList

An object representing a list of runs for an evaluation. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The type of this object. It is always set to \&quot;list\&quot;.  | [default to 'list']
**data** | [**List[EvalRun]**](EvalRun.md) | An array of eval run objects.  | 
**first_id** | **str** | The identifier of the first eval run in the data array. | 
**last_id** | **str** | The identifier of the last eval run in the data array. | 
**has_more** | **bool** | Indicates whether there are more evals available. | 

## Example

```python
from openapi_client.models.eval_run_list import EvalRunList

# TODO update the JSON string below
json = "{}"
# create an instance of EvalRunList from a JSON string
eval_run_list_instance = EvalRunList.from_json(json)
# print the JSON string representation of the object
print(EvalRunList.to_json())

# convert the object into a dict
eval_run_list_dict = eval_run_list_instance.to_dict()
# create an instance of EvalRunList from a dict
eval_run_list_from_dict = EvalRunList.from_dict(eval_run_list_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


