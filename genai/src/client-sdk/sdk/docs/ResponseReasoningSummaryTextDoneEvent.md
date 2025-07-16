# ResponseReasoningSummaryTextDoneEvent

Emitted when a reasoning summary text is completed.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.reasoning_summary_text.done&#x60;.  | 
**item_id** | **str** | The ID of the item this summary text is associated with.  | 
**output_index** | **int** | The index of the output item this summary text is associated with.  | 
**summary_index** | **int** | The index of the summary part within the reasoning summary.  | 
**text** | **str** | The full text of the completed reasoning summary.  | 

## Example

```python
from openapi_client.models.response_reasoning_summary_text_done_event import ResponseReasoningSummaryTextDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseReasoningSummaryTextDoneEvent from a JSON string
response_reasoning_summary_text_done_event_instance = ResponseReasoningSummaryTextDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseReasoningSummaryTextDoneEvent.to_json())

# convert the object into a dict
response_reasoning_summary_text_done_event_dict = response_reasoning_summary_text_done_event_instance.to_dict()
# create an instance of ResponseReasoningSummaryTextDoneEvent from a dict
response_reasoning_summary_text_done_event_from_dict = ResponseReasoningSummaryTextDoneEvent.from_dict(response_reasoning_summary_text_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


