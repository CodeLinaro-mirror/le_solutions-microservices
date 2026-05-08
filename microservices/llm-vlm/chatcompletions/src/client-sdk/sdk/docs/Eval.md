# Eval

An Eval object with a data source config and testing criteria. An Eval represents a task to be done for your LLM integration. Like:  - Improve the quality of my chatbot  - See how well my chatbot handles customer support 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**object** | **str** | The object type. | [default to 'eval']
**id** | **str** | Unique identifier for the evaluation. | 
**name** | **str** | The name of the evaluation. | 
**data_source_config** | [**EvalDataSourceConfig**](EvalDataSourceConfig.md) |  | 
**testing_criteria** | [**List[EvalTestingCriteriaInner]**](EvalTestingCriteriaInner.md) | A list of testing criteria. | 
**created_at** | **int** | The Unix timestamp (in seconds) for when the eval was created. | 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | 

## Example

```python
from openapi_client.models.eval import Eval

# TODO update the JSON string below
json = "{}"
# create an instance of Eval from a JSON string
eval_instance = Eval.from_json(json)
# print the JSON string representation of the object
print(Eval.to_json())

# convert the object into a dict
eval_dict = eval_instance.to_dict()
# create an instance of Eval from a dict
eval_from_dict = Eval.from_dict(eval_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


