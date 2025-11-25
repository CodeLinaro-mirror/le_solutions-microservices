# WebSearchPreviewTool

This tool searches the web for relevant results to use in a response.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the web search tool. One of &#x60;web_search_preview&#x60; or &#x60;web_search_preview_2025_03_11&#x60;. | [default to 'web_search_preview']
**user_location** | [**ApproximateLocation**](ApproximateLocation.md) |  | [optional] 
**search_context_size** | **str** | High level guidance for the amount of context window space to use for the search. One of &#x60;low&#x60;, &#x60;medium&#x60;, or &#x60;high&#x60;. &#x60;medium&#x60; is the default. | [optional] 

## Example

```python
from openapi_client.models.web_search_preview_tool import WebSearchPreviewTool

# TODO update the JSON string below
json = "{}"
# create an instance of WebSearchPreviewTool from a JSON string
web_search_preview_tool_instance = WebSearchPreviewTool.from_json(json)
# print the JSON string representation of the object
print(WebSearchPreviewTool.to_json())

# convert the object into a dict
web_search_preview_tool_dict = web_search_preview_tool_instance.to_dict()
# create an instance of WebSearchPreviewTool from a dict
web_search_preview_tool_from_dict = WebSearchPreviewTool.from_dict(web_search_preview_tool_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


