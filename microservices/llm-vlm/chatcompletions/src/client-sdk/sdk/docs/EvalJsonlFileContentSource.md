# EvalJsonlFileContentSource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of jsonl source. Always &#x60;file_content&#x60;. | [default to 'file_content']
**content** | [**List[EvalJsonlFileContentSourceContentInner]**](EvalJsonlFileContentSourceContentInner.md) | The content of the jsonl file. | 

## Example

```python
from openapi_client.models.eval_jsonl_file_content_source import EvalJsonlFileContentSource

# TODO update the JSON string below
json = "{}"
# create an instance of EvalJsonlFileContentSource from a JSON string
eval_jsonl_file_content_source_instance = EvalJsonlFileContentSource.from_json(json)
# print the JSON string representation of the object
print(EvalJsonlFileContentSource.to_json())

# convert the object into a dict
eval_jsonl_file_content_source_dict = eval_jsonl_file_content_source_instance.to_dict()
# create an instance of EvalJsonlFileContentSource from a dict
eval_jsonl_file_content_source_from_dict = EvalJsonlFileContentSource.from_dict(eval_jsonl_file_content_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


