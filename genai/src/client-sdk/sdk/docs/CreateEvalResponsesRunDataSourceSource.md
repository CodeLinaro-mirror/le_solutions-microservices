# CreateEvalResponsesRunDataSourceSource


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of jsonl source. Always &#x60;file_content&#x60;. | [default to 'file_content']
**content** | [**List[EvalJsonlFileContentSourceContentInner]**](EvalJsonlFileContentSourceContentInner.md) | The content of the jsonl file. | 
**id** | **str** | The identifier of the file. | 
**metadata** | **object** | Metadata filter for the responses. This is a query parameter used to select responses. | [optional] 
**model** | **str** | The name of the model to find responses for. This is a query parameter used to select responses. | [optional] 
**instructions_search** | **str** | Optional search string for instructions. This is a query parameter used to select responses. | [optional] 
**created_after** | **int** | Only include items created after this timestamp (inclusive). This is a query parameter used to select responses. | [optional] 
**created_before** | **int** | Only include items created before this timestamp (inclusive). This is a query parameter used to select responses. | [optional] 
**has_tool_calls** | **bool** | Whether the response has tool calls. This is a query parameter used to select responses. | [optional] 
**reasoning_effort** | [**ReasoningEffort**](ReasoningEffort.md) |  | [optional] [default to ReasoningEffort.MEDIUM]
**temperature** | **float** | Sampling temperature. This is a query parameter used to select responses. | [optional] 
**top_p** | **float** | Nucleus sampling parameter. This is a query parameter used to select responses. | [optional] 
**users** | **List[str]** | List of user identifiers. This is a query parameter used to select responses. | [optional] 
**allow_parallel_tool_calls** | **bool** | Whether to allow parallel tool calls. This is a query parameter used to select responses. | [optional] 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source_source import CreateEvalResponsesRunDataSourceSource

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSourceSource from a JSON string
create_eval_responses_run_data_source_source_instance = CreateEvalResponsesRunDataSourceSource.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSourceSource.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_source_dict = create_eval_responses_run_data_source_source_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSourceSource from a dict
create_eval_responses_run_data_source_source_from_dict = CreateEvalResponsesRunDataSourceSource.from_dict(create_eval_responses_run_data_source_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


