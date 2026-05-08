# ResponseTextDoneEvent

Emitted when text content is finalized.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.output_text.done&#x60;.  | 
**item_id** | **str** | The ID of the output item that the text content is finalized.  | 
**output_index** | **int** | The index of the output item that the text content is finalized.  | 
**content_index** | **int** | The index of the content part that the text content is finalized.  | 
**text** | **str** | The text content that is finalized.  | 

## Example

```python
from openapi_client.models.response_text_done_event import ResponseTextDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseTextDoneEvent from a JSON string
response_text_done_event_instance = ResponseTextDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseTextDoneEvent.to_json())

# convert the object into a dict
response_text_done_event_dict = response_text_done_event_instance.to_dict()
# create an instance of ResponseTextDoneEvent from a dict
response_text_done_event_from_dict = ResponseTextDoneEvent.from_dict(response_text_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


