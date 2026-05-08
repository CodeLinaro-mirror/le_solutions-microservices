# WebSearch

This tool searches the web for relevant results to use in a response. Learn more about the [web search tool](/docs/guides/tools-web-search?api-mode=chat). 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**user_location** | [**WebSearchUserLocation**](WebSearchUserLocation.md) |  | [optional] 
**search_context_size** | [**WebSearchContextSize**](WebSearchContextSize.md) |  | [optional] [default to WebSearchContextSize.MEDIUM]

## Example

```python
from openapi_client.models.web_search import WebSearch

# TODO update the JSON string below
json = "{}"
# create an instance of WebSearch from a JSON string
web_search_instance = WebSearch.from_json(json)
# print the JSON string representation of the object
print(WebSearch.to_json())

# convert the object into a dict
web_search_dict = web_search_instance.to_dict()
# create an instance of WebSearch from a dict
web_search_from_dict = WebSearch.from_dict(web_search_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


