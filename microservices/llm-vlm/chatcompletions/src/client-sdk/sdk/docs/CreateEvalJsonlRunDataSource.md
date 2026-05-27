# CreateEvalJsonlRunDataSource

A JsonlRunDataSource object with that specifies a JSONL file that matches the eval  

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of data source. Always &#x60;jsonl&#x60;. | [default to 'jsonl']
**source** | [**CreateEvalJsonlRunDataSourceSource**](CreateEvalJsonlRunDataSourceSource.md) |  | 

## Example

```python
from openapi_client.models.create_eval_jsonl_run_data_source import CreateEvalJsonlRunDataSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalJsonlRunDataSource from a JSON string
create_eval_jsonl_run_data_source_instance = CreateEvalJsonlRunDataSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalJsonlRunDataSource.to_json())

# convert the object into a dict
create_eval_jsonl_run_data_source_dict = create_eval_jsonl_run_data_source_instance.to_dict()
# create an instance of CreateEvalJsonlRunDataSource from a dict
create_eval_jsonl_run_data_source_from_dict = CreateEvalJsonlRunDataSource.from_dict(create_eval_jsonl_run_data_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


