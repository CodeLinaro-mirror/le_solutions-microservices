# ResponseFileSearchCallInProgressEvent

Emitted when a file search call is initiated.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.file_search_call.in_progress&#x60;.  | 
**output_index** | **int** | The index of the output item that the file search call is initiated.  | 
**item_id** | **str** | The ID of the output item that the file search call is initiated.  | 

## Example

```python
from openapi_client.models.response_file_search_call_in_progress_event import ResponseFileSearchCallInProgressEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseFileSearchCallInProgressEvent from a JSON string
response_file_search_call_in_progress_event_instance = ResponseFileSearchCallInProgressEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseFileSearchCallInProgressEvent.to_json())

# convert the object into a dict
response_file_search_call_in_progress_event_dict = response_file_search_call_in_progress_event_instance.to_dict()
# create an instance of ResponseFileSearchCallInProgressEvent from a dict
response_file_search_call_in_progress_event_from_dict = ResponseFileSearchCallInProgressEvent.from_dict(response_file_search_call_in_progress_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


