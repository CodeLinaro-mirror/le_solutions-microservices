# WebSearchToolCall

The results of a web search tool call. See the  [web search guide](/docs/guides/tools-web-search) for more information. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**id** | **str** | The unique ID of the web search tool call.  | 
**type** | **str** | The type of the web search tool call. Always &#x60;web_search_call&#x60;.  | 
**status** | **str** | The status of the web search tool call.  | 

## Example

```python
from openapi_client.models.web_search_tool_call import WebSearchToolCall

# TODO update the JSON string below
json = "{}"
# create an instance of WebSearchToolCall from a JSON string
web_search_tool_call_instance = WebSearchToolCall.from_json(json)
# print the JSON string representation of the object
print(WebSearchToolCall.to_json())

# convert the object into a dict
web_search_tool_call_dict = web_search_tool_call_instance.to_dict()
# create an instance of WebSearchToolCall from a dict
web_search_tool_call_from_dict = WebSearchToolCall.from_dict(web_search_tool_call_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


