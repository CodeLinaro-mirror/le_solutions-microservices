# EvalStoredCompletionsSource

A StoredCompletionsRunDataSource configuration describing a set of filters 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of source. Always &#x60;stored_completions&#x60;. | [default to 'stored_completions']
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**model** | **str** | An optional model to filter by. | [optional] 
**created_after** | **int** | An optional Unix timestamp to filter items created after this time. | [optional] 
**created_before** | **int** | An optional Unix timestamp to filter items created before this time. | [optional] 
**limit** | **int** | An optional maximum number of items to return. | [optional] 

## Example

```python
from openapi_client.models.eval_stored_completions_source import EvalStoredCompletionsSource

# TODO update the JSON string below
json = "{}"
# create an instance of EvalStoredCompletionsSource from a JSON string
eval_stored_completions_source_instance = EvalStoredCompletionsSource.from_json(json)
# print the JSON string representation of the object
print(EvalStoredCompletionsSource.to_json())

# convert the object into a dict
eval_stored_completions_source_dict = eval_stored_completions_source_instance.to_dict()
# create an instance of EvalStoredCompletionsSource from a dict
eval_stored_completions_source_from_dict = EvalStoredCompletionsSource.from_dict(eval_stored_completions_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


