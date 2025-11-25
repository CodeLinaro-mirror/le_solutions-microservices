# CreateEvalCompletionsRunDataSourceSource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of jsonl source. Always &#x60;file_content&#x60;. | [default to 'file_content']
**content** | [**List[EvalJsonlFileContentSourceContentInner]**](EvalJsonlFileContentSourceContentInner.md) | The content of the jsonl file. | 
**id** | **str** | The identifier of the file. | 
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**model** | **str** | An optional model to filter by. | [optional] 
**created_after** | **int** | An optional Unix timestamp to filter items created after this time. | [optional] 
**created_before** | **int** | An optional Unix timestamp to filter items created before this time. | [optional] 
**limit** | **int** | An optional maximum number of items to return. | [optional] 

## Example

```python
from openapi_client.models.create_eval_completions_run_data_source_source import CreateEvalCompletionsRunDataSourceSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalCompletionsRunDataSourceSource from a JSON string
create_eval_completions_run_data_source_source_instance = CreateEvalCompletionsRunDataSourceSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalCompletionsRunDataSourceSource.to_json())

# convert the object into a dict
create_eval_completions_run_data_source_source_dict = create_eval_completions_run_data_source_source_instance.to_dict()
# create an instance of CreateEvalCompletionsRunDataSourceSource from a dict
create_eval_completions_run_data_source_source_from_dict = CreateEvalCompletionsRunDataSourceSource.from_dict(create_eval_completions_run_data_source_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


