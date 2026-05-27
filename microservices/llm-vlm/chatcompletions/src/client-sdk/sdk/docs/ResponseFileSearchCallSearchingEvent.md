# ResponseFileSearchCallSearchingEvent

Emitted when a file search is currently searching.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.file_search_call.searching&#x60;.  | 
**output_index** | **int** | The index of the output item that the file search call is searching.  | 
**item_id** | **str** | The ID of the output item that the file search call is initiated.  | 

## Example

```python
from openapi_client.models.response_file_search_call_searching_event import ResponseFileSearchCallSearchingEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseFileSearchCallSearchingEvent from a JSON string
response_file_search_call_searching_event_instance = ResponseFileSearchCallSearchingEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseFileSearchCallSearchingEvent.to_json())

# convert the object into a dict
response_file_search_call_searching_event_dict = response_file_search_call_searching_event_instance.to_dict()
# create an instance of ResponseFileSearchCallSearchingEvent from a dict
response_file_search_call_searching_event_from_dict = ResponseFileSearchCallSearchingEvent.from_dict(response_file_search_call_searching_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


