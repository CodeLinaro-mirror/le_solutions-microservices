# ResponseWebSearchCallCompletedEvent

Emitted when a web search call is completed.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.web_search_call.completed&#x60;.  | 
**output_index** | **int** | The index of the output item that the web search call is associated with.  | 
**item_id** | **str** | Unique ID for the output item associated with the web search call.  | 

## Example

```python
from openapi_client.models.response_web_search_call_completed_event import ResponseWebSearchCallCompletedEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseWebSearchCallCompletedEvent from a JSON string
response_web_search_call_completed_event_instance = ResponseWebSearchCallCompletedEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseWebSearchCallCompletedEvent.to_json())

# convert the object into a dict
response_web_search_call_completed_event_dict = response_web_search_call_completed_event_instance.to_dict()
# create an instance of ResponseWebSearchCallCompletedEvent from a dict
response_web_search_call_completed_event_from_dict = ResponseWebSearchCallCompletedEvent.from_dict(response_web_search_call_completed_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


