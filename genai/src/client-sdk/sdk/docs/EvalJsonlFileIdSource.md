# EvalJsonlFileIdSource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of jsonl source. Always &#x60;file_id&#x60;. | [default to 'file_id']
**id** | **str** | The identifier of the file. | 

## Example

```python
from openapi_client.models.eval_jsonl_file_id_source import EvalJsonlFileIdSource

# TODO update the JSON string below
json = "{}"
# create an instance of EvalJsonlFileIdSource from a JSON string
eval_jsonl_file_id_source_instance = EvalJsonlFileIdSource.from_json(json)
# print the JSON string representation of the object
print(EvalJsonlFileIdSource.to_json())

# convert the object into a dict
eval_jsonl_file_id_source_dict = eval_jsonl_file_id_source_instance.to_dict()
# create an instance of EvalJsonlFileIdSource from a dict
eval_jsonl_file_id_source_from_dict = EvalJsonlFileIdSource.from_dict(eval_jsonl_file_id_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


