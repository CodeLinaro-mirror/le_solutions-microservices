# CreateEvalJsonlRunDataSourceSource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of jsonl source. Always &#x60;file_content&#x60;. | [default to 'file_content']
**content** | [**List[EvalJsonlFileContentSourceContentInner]**](EvalJsonlFileContentSourceContentInner.md) | The content of the jsonl file. | 
**id** | **str** | The identifier of the file. | 

## Example

```python
from openapi_client.models.create_eval_jsonl_run_data_source_source import CreateEvalJsonlRunDataSourceSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalJsonlRunDataSourceSource from a JSON string
create_eval_jsonl_run_data_source_source_instance = CreateEvalJsonlRunDataSourceSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalJsonlRunDataSourceSource.to_json())

# convert the object into a dict
create_eval_jsonl_run_data_source_source_dict = create_eval_jsonl_run_data_source_source_instance.to_dict()
# create an instance of CreateEvalJsonlRunDataSourceSource from a dict
create_eval_jsonl_run_data_source_source_from_dict = CreateEvalJsonlRunDataSourceSource.from_dict(create_eval_jsonl_run_data_source_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


