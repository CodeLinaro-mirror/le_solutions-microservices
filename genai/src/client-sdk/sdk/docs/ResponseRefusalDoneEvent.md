# ResponseRefusalDoneEvent

Emitted when refusal text is finalized.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.refusal.done&#x60;.  | 
**item_id** | **str** | The ID of the output item that the refusal text is finalized.  | 
**output_index** | **int** | The index of the output item that the refusal text is finalized.  | 
**content_index** | **int** | The index of the content part that the refusal text is finalized.  | 
**refusal** | **str** | The refusal text that is finalized.  | 

## Example

```python
from openapi_client.models.response_refusal_done_event import ResponseRefusalDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseRefusalDoneEvent from a JSON string
response_refusal_done_event_instance = ResponseRefusalDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseRefusalDoneEvent.to_json())

# convert the object into a dict
response_refusal_done_event_dict = response_refusal_done_event_instance.to_dict()
# create an instance of ResponseRefusalDoneEvent from a dict
response_refusal_done_event_from_dict = ResponseRefusalDoneEvent.from_dict(response_refusal_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


