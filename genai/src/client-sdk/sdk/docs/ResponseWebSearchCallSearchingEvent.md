# ResponseWebSearchCallSearchingEvent

Emitted when a web search call is executing.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.web_search_call.searching&#x60;.  | 
**output_index** | **int** | The index of the output item that the web search call is associated with.  | 
**item_id** | **str** | Unique ID for the output item associated with the web search call.  | 

## Example

```python
from openapi_client.models.response_web_search_call_searching_event import ResponseWebSearchCallSearchingEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseWebSearchCallSearchingEvent from a JSON string
response_web_search_call_searching_event_instance = ResponseWebSearchCallSearchingEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseWebSearchCallSearchingEvent.to_json())

# convert the object into a dict
response_web_search_call_searching_event_dict = response_web_search_call_searching_event_instance.to_dict()
# create an instance of ResponseWebSearchCallSearchingEvent from a dict
response_web_search_call_searching_event_from_dict = ResponseWebSearchCallSearchingEvent.from_dict(response_web_search_call_searching_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


