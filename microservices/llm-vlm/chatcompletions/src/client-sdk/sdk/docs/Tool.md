# Tool


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the file search tool. Always &#x60;file_search&#x60;. | [default to 'file_search']
**vector_store_ids** | **List[str]** | The IDs of the vector stores to search. | 
**max_num_results** | **int** | The maximum number of results to return. This number should be between 1 and 50 inclusive. | [optional] 
**ranking_options** | [**RankingOptions**](RankingOptions.md) |  | [optional] 
**filters** | [**Filters**](Filters.md) |  | [optional] 
**name** | **str** | The name of the function to call. | 
**description** | **str** | A description of the function. Used by the model to determine whether or not to call the function. | [optional] 
**parameters** | **Dict[str, object]** | A JSON schema object describing the parameters of the function. | 
**strict** | **bool** | Whether to enforce strict parameter validation. Default &#x60;true&#x60;. | 
**user_location** | [**ApproximateLocation**](ApproximateLocation.md) |  | [optional] 
**search_context_size** | **str** | High level guidance for the amount of context window space to use for the search. One of &#x60;low&#x60;, &#x60;medium&#x60;, or &#x60;high&#x60;. &#x60;medium&#x60; is the default. | [optional] 
**environment** | **str** | The type of computer environment to control. | 
**display_width** | **int** | The width of the computer display. | 
**display_height** | **int** | The height of the computer display. | 

## Example

```python
from openapi_client.models.tool import Tool

# TODO update the JSON string below
json = "{}"
# create an instance of Tool from a JSON string
tool_instance = Tool.from_json(json)
# print the JSON string representation of the object
print(Tool.to_json())

# convert the object into a dict
tool_dict = tool_instance.to_dict()
# create an instance of Tool from a dict
tool_from_dict = Tool.from_dict(tool_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


