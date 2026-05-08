# EvalResponsesSource

A EvalResponsesSource object describing a run data source configuration. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of run data source. Always &#x60;responses&#x60;. | 
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
from openapi_client.models.eval_responses_source import EvalResponsesSource

# TODO update the JSON string below
json = "{}"
# create an instance of EvalResponsesSource from a JSON string
eval_responses_source_instance = EvalResponsesSource.from_json(json)
# print the JSON string representation of the object
print(EvalResponsesSource.to_json())

# convert the object into a dict
eval_responses_source_dict = eval_responses_source_instance.to_dict()
# create an instance of EvalResponsesSource from a dict
eval_responses_source_from_dict = EvalResponsesSource.from_dict(eval_responses_source_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


